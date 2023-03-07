/*
 * Copyright 2019 Broadcom. The term "Broadcom" refers to Broadcom Inc. and/or
 * its subsidiaries.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fstream>

#include "stpmgr.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "select.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

int gBatchSize = 0;
bool gSwssRecord = false;
bool gLogRotate = false;
ofstream gRecordOfs;
string gRecordFile;

#define SELECT_TIMEOUT 1000

int main(int argc, char **argv)
{
    Logger::linkToDbNative("stpmgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting stpmgrd ---");

    if (fopen("/stpmgrd_dbg_reload", "r"))
    {
        Logger::setMinPrio(Logger::SWSS_DEBUG);
    }

    try
    {
        DBConnector conf_db(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
        DBConnector app_db(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
        DBConnector state_db(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);

        WarmStart::initialize("stpmgrd", "stpd");
        WarmStart::checkWarmStart("stpmgrd", "stpd");

        // Config DB Tables
        TableConnector conf_stp_global_table(&conf_db, CFG_STP_GLOBAL_TABLE_NAME);
        TableConnector conf_stp_vlan_table(&conf_db, CFG_STP_VLAN_TABLE_NAME);
        TableConnector conf_stp_vlan_port_table(&conf_db, CFG_STP_VLAN_PORT_TABLE_NAME);
        TableConnector conf_stp_port_table(&conf_db, CFG_STP_PORT_TABLE_NAME);

        // VLAN DB Tables
        TableConnector state_vlan_member_table(&state_db, STATE_VLAN_MEMBER_TABLE_NAME);

        // LAG Tables
        TableConnector conf_lag_member_table(&conf_db, CFG_LAG_MEMBER_TABLE_NAME);

        vector<TableConnector> tables = {
            conf_stp_global_table,
            conf_stp_vlan_table,
            conf_stp_vlan_port_table,
            conf_stp_port_table,
            conf_lag_member_table,
            state_vlan_member_table
        };


        StpMgr stpmgr(&conf_db, &app_db, &state_db, tables);

        // Open a Unix Domain Socket with STPd
        stpmgr.ipcInitStpd();
        stpmgr.isPortInitDone(&app_db);
        
        // Get max STP instances from state DB and send to stpd
        STP_INIT_READY_MSG msg;
        memset(&msg, 0, sizeof(STP_INIT_READY_MSG));
        msg.max_stp_instances = stpmgr.getStpMaxInstances();
        stpmgr.sendMsgStpd(STP_INIT_READY, sizeof(msg), (void *)&msg);

        // Get Base MAC
        Table table(&conf_db, "DEVICE_METADATA");
        std::vector<FieldValueTuple> ovalues;
        table.get("localhost", ovalues);
        auto it = std::find_if( ovalues.begin(), ovalues.end(), [](const FieldValueTuple& t){ return t.first == "mac";} );
        if ( it == ovalues.end() ) {
            throw runtime_error("couldn't find MAC address of the device from config DB");
        }
        stpmgr.macAddress = MacAddress(it->second);

        vector<Orch *> cfgOrchList = {&stpmgr};

        Select s;
        for (Orch *o: cfgOrchList)
        {
            s.addSelectables(o->getSelectables());
        }

        while (true)
        {
            Selectable *sel;
            int ret;

            ret = s.select(&sel, SELECT_TIMEOUT);
            if (ret == Select::ERROR)
            {
                SWSS_LOG_NOTICE("Error: %s!", strerror(errno));
                continue;
            }
            if (ret == Select::TIMEOUT)
            {
                stpmgr.doTask();
                continue;
            }

            auto *c = (Executor *)sel;
            c->execute();
        }
    }
    catch (const exception &e)
    {
        SWSS_LOG_ERROR("Runtime error: %s", e.what());
    }

    return -1;
}
