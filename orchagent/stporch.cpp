#include <tuple>
#include "portsorch.h"
#include "logger.h"
#include "fdborch.h"
#include "stporch.h"

extern sai_stp_api_t *sai_stp_api;
extern sai_vlan_api_t *sai_vlan_api;
extern sai_switch_api_t *sai_switch_api;

extern FdbOrch *gFdbOrch;
extern PortsOrch*        gPortsOrch;

extern sai_object_id_t gSwitchId;

StpOrch::StpOrch(DBConnector * db, DBConnector * stateDb, vector<string> &tableNames) :
    Orch(db, tableNames)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    sai_status_t status;

    m_stpTable = unique_ptr<Table>(new Table(stateDb, STATE_STP_TABLE_NAME));
    
    vector<sai_attribute_t> attrs;
    attr.id = SAI_SWITCH_ATTR_DEFAULT_STP_INST_ID;
    attrs.push_back(attr);
    //attr.id = SAI_SWITCH_ATTR_MAX_STP_INSTANCE;
    //attrs.push_back(attr);
    
    status = sai_switch_api->get_switch_attribute(gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get default STP instance and max STP instances , rv:%d", status);
        throw runtime_error("StpOrch initialization failure");
    }
    
    m_defaultStpId = attrs[0].value.oid;
    //updateMaxStpInstance(attrs[1].value.u32);
};


sai_object_id_t StpOrch::getStpInstanceOid(sai_uint16_t stp_instance)
{
    std::map<sai_uint16_t, sai_object_id_t>::iterator it;

    it = m_stpInstToOid.find(stp_instance);
    if (it == m_stpInstToOid.end())
    {
        return SAI_NULL_OBJECT_ID;
    }

    return it->second;
}

sai_object_id_t StpOrch::addStpInstance(sai_uint16_t stp_instance)
{
    sai_object_id_t stp_oid;
    sai_attribute_t attr;
    
    sai_status_t status = sai_stp_api->create_stp(&stp_oid, gSwitchId, 0, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create STP instance %u status %u", stp_instance, status);
        return SAI_NULL_OBJECT_ID;
    }
    m_stpInstToOid[stp_instance] = stp_oid;
    SWSS_LOG_NOTICE("Added STP instance:%hu oid:%lx", stp_instance, stp_oid);
    return stp_oid;
}

bool StpOrch::removeStpInstance(sai_uint16_t stp_instance)
{
    sai_object_id_t stp_oid;

    stp_oid = getStpInstanceOid(stp_instance);
    if (stp_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Failed to find oid for STP instance %u", stp_instance);
        return false;
    }
    
    /* Remove all STP ports before deleting the STP instance */
    auto portList = gPortsOrch->getAllPorts();
    for (auto &it: portList)
    {
        auto &port = it.second;
        if (port.m_type == Port::PHY || port.m_type == Port::LAG)
        {
            if(port.m_stp_port_ids.find(stp_instance) == port.m_stp_port_ids.end())
                continue;

            removeStpPort(port, stp_instance);
        }
    }

    sai_status_t status = sai_stp_api->remove_stp(stp_oid);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove STP instance %u oid %lx status %u", stp_instance, stp_oid, status);
        return false;
    }

    m_stpInstToOid.erase(stp_instance);
    SWSS_LOG_NOTICE("Removed STP instance:%hu oid:%lx", stp_instance, stp_oid);
    return true;
}

bool StpOrch::addVlanToStpInstance(string vlan_alias, sai_uint16_t stp_instance)
{
    SWSS_LOG_ENTER();

    Port vlan;
    sai_object_id_t stp_oid;
    sai_attribute_t attr;

    if (!gPortsOrch->getPort(vlan_alias, vlan))
    {
        SWSS_LOG_INFO("Failed to locate VLAN %s", vlan_alias.c_str());
        return false;
    }
    
    stp_oid = getStpInstanceOid(stp_instance);
    if (stp_oid == SAI_NULL_OBJECT_ID)
    {
        stp_oid = addStpInstance(stp_instance);
        if(stp_oid == SAI_NULL_OBJECT_ID)
            return false;
    }

    attr.id = SAI_VLAN_ATTR_STP_INSTANCE;
    attr.value.oid = stp_oid;

    sai_status_t status = sai_vlan_api->set_vlan_attribute(vlan.m_vlan_info.vlan_oid, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to add VLAN %s to STP instance:%hu status %u", vlan_alias.c_str(), stp_instance, status);
        return false;
    }

    vlan.m_stp_id = stp_instance;
    gPortsOrch->setPort(vlan_alias, vlan);
    SWSS_LOG_NOTICE("Add VLAN %s to STP instance:%hu m_stp_id:%d", vlan_alias.c_str(), stp_instance, vlan.m_stp_id);
    return true;
}

bool StpOrch::removeVlanFromStpInstance(string vlan_alias, sai_uint16_t stp_instance)
{
    SWSS_LOG_ENTER();

    Port vlan;
    sai_attribute_t attr;

    if (!gPortsOrch->getPort(vlan_alias, vlan))
    {
        SWSS_LOG_INFO("Failed to locate VLAN %s", vlan_alias.c_str());
        return false;
    }

    attr.id = SAI_VLAN_ATTR_STP_INSTANCE;
    attr.value.oid = m_defaultStpId;

    sai_status_t status = sai_vlan_api->set_vlan_attribute(vlan.m_vlan_info.vlan_oid, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to add VLAN %s to STP instance:%d status %u", vlan_alias.c_str(), vlan.m_stp_id, status);
        return false;
    }

    SWSS_LOG_NOTICE("Remove %s from instance:%d add instance:%lx", vlan_alias.c_str(), vlan.m_stp_id, m_defaultStpId);
    
    removeStpInstance(vlan.m_stp_id);
    vlan.m_stp_id = -1;
    gPortsOrch->setPort(vlan_alias, vlan);
    return true;
}

/* If STP Port exists return else create a new STP Port */
sai_object_id_t StpOrch::addStpPort(Port &port, sai_uint16_t stp_instance)
{
    sai_object_id_t stp_port_id = SAI_NULL_OBJECT_ID;
    sai_object_id_t stp_id = SAI_NULL_OBJECT_ID;
    sai_attribute_t attr[3];

    if(port.m_stp_port_ids.find(stp_instance) != port.m_stp_port_ids.end())
    {
        return port.m_stp_port_ids[stp_instance];
    }

    if(port.m_bridge_port_id == SAI_NULL_OBJECT_ID)
    {
        gPortsOrch->addBridgePort(port);

        if(port.m_bridge_port_id == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("Failed to add STP port %s invalid bridge port id STP instance %d", port.m_alias.c_str(), stp_instance);
            return SAI_NULL_OBJECT_ID;
        }
    }

    attr[0].id = SAI_STP_PORT_ATTR_BRIDGE_PORT;
    attr[0].value.oid = port.m_bridge_port_id;
    
    stp_id = getStpInstanceOid(stp_instance);
    if(stp_id == SAI_NULL_OBJECT_ID)
    {
        stp_id = addStpInstance(stp_instance);
        if(stp_id == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("Failed to add STP instance %d for port %s", stp_instance, port.m_alias.c_str());
            return SAI_NULL_OBJECT_ID;
        }
    }

    attr[1].id = SAI_STP_PORT_ATTR_STP;
    attr[1].value.oid = stp_id;
    
    attr[2].id = SAI_STP_PORT_ATTR_STATE;
    attr[2].value.s32 = SAI_STP_PORT_STATE_BLOCKING;

    sai_status_t status = sai_stp_api->create_stp_port(&stp_port_id, gSwitchId, 3, attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to add STP port %s instance %d status %u", port.m_alias.c_str(), stp_instance, status);
        return SAI_NULL_OBJECT_ID;
    }

    SWSS_LOG_NOTICE("Add STP port %s instance %d oid %lx size %lu", port.m_alias.c_str(), stp_instance, stp_port_id, port.m_stp_port_ids.size());

    port.m_stp_port_ids[stp_instance] = stp_port_id;
    gPortsOrch->setPort(port.m_alias, port);
    return stp_port_id;
}

bool StpOrch::removeStpPort(Port &port, sai_uint16_t stp_instance)
{
    if(port.m_stp_port_ids.find(stp_instance) == port.m_stp_port_ids.end())
    {
        /* Deletion could have already happened as part of other flows, so ignore this msg*/
        return true;
    }

    sai_status_t status = sai_stp_api->remove_stp_port(port.m_stp_port_ids[stp_instance]);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove STP port %s instance %d oid %lx status %x", port.m_alias.c_str(), stp_instance, 
                port.m_stp_port_ids[stp_instance], status);
        return false;
    }

    SWSS_LOG_NOTICE("Remove STP port %s instance %d oid %lx size %lu", port.m_alias.c_str(), stp_instance, 
            port.m_stp_port_ids[stp_instance], port.m_stp_port_ids.size());
    port.m_stp_port_ids.erase(stp_instance);
    gPortsOrch->setPort(port.m_alias, port);
    return true;
}

bool StpOrch::removeStpPorts(Port &port)
{
    if(port.m_stp_port_ids.empty())
        return true;

    for(auto stp_port_id: port.m_stp_port_ids)
    {
        uint16_t stp_instance = stp_port_id.first;
        sai_object_id_t stp_port_oid = stp_port_id.second;

        if(stp_port_oid == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("Failed to find STP port %s instance %d", port.m_alias.c_str(), stp_instance);
            continue;
        }

        sai_status_t status = sai_stp_api->remove_stp_port(stp_port_oid);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove STP port %s instance %d oid %lx status %x", port.m_alias.c_str(), stp_instance, stp_port_oid, status);
        }
        else
        {
            SWSS_LOG_NOTICE("Remove STP port %s instance %d oid %lx", port.m_alias.c_str(), stp_instance, stp_port_oid);
        }
    }

    port.m_stp_port_ids.clear();
    gPortsOrch->setPort(port.m_alias, port);
    return true;
}

sai_stp_port_state_t StpOrch::getStpSaiState(sai_uint8_t stp_state)
{
    sai_stp_port_state_t state = SAI_STP_PORT_STATE_BLOCKING;

    switch(stp_state)
    {
        case STP_STATE_DISABLED:
        case STP_STATE_BLOCKING:
        case STP_STATE_LISTENING:
            state = SAI_STP_PORT_STATE_BLOCKING;
            break;

        case STP_STATE_LEARNING:
            state = SAI_STP_PORT_STATE_LEARNING;
            break;

        case STP_STATE_FORWARDING:
            state = SAI_STP_PORT_STATE_FORWARDING;
            break;
    }
    return state;
}

bool StpOrch::updateStpPortState(Port &port, sai_uint16_t stp_instance, sai_uint8_t stp_state)
{
    sai_attribute_t attr[1];
    sai_object_id_t stp_port_oid;

    stp_port_oid = addStpPort(port, stp_instance);

    if (stp_port_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Failed to get STP port oid port %s instance %d state %d ", port.m_alias.c_str(), stp_instance, stp_state);
        return true;
    }

    attr[0].id = SAI_STP_PORT_ATTR_STATE;
    attr[0].value.u32 = getStpSaiState(stp_state);

    sai_status_t status = sai_stp_api->set_stp_port_attribute(stp_port_oid, attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set STP port state %s instance %d state %d status %x", port.m_alias.c_str(), stp_instance, stp_state, status);
        return false;
    }
    
    SWSS_LOG_NOTICE("Set STP port state %s instance %d state %d ", port.m_alias.c_str(), stp_instance, stp_state);

    return true;
}

bool StpOrch::stpVlanFdbFlush(string vlan_alias)
{
    SWSS_LOG_ENTER();

    Port vlan;

    if (!gPortsOrch->getPort(vlan_alias, vlan))
    {
        SWSS_LOG_INFO("Failed to locate VLAN %s", vlan_alias.c_str());
        return false;
    }

    //gFdbOrch->flushFdbByVlan(vlan_alias, 0);
    
    SWSS_LOG_NOTICE("Set STP FDB flush vlan %s ", vlan_alias.c_str());
    return true;
}

bool StpOrch::updateMaxStpInstance(uint32_t max_stp_instances)
{
    SWSS_LOG_NOTICE("Max STP instances %d", (max_stp_instances - 1));

    vector<FieldValueTuple> tuples;
    FieldValueTuple tuple("max_stp_inst", to_string(max_stp_instances - 1));
    tuples.push_back(tuple);
    m_stpTable->set("GLOBAL", tuples);

    return true;
}

void StpOrch::doStpTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;

        string vlan_alias = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            string stp_instance;
            uint16_t instance = STP_INVALID_INSTANCE;

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "stp_instance")
                {
                    stp_instance = fvValue(i);
                    instance = (uint16_t)std::stoi(stp_instance);
                }
            }

            if(instance == STP_INVALID_INSTANCE)
            {
                SWSS_LOG_ERROR("No instance found for VLAN %s", vlan_alias.c_str());
            }
            else
            {
                if(!addVlanToStpInstance(vlan_alias, instance))
                {
                    it++;
                    continue;
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            if(!removeVlanFromStpInstance(vlan_alias, 0))
            {
                it++;
                continue;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }
        it = consumer.m_toSync.erase(it);
    }
}

void StpOrch::doStpPortStateTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;
        string key = kfvKey(t);
        size_t found = key.find(':');
        /* Return if the format of key is wrong */
        if (found == string::npos)
        {
            SWSS_LOG_ERROR("Failed to parse %s", key.c_str());
            return;
        }
        string port_alias = key.substr(0, found);
        string stp_instance = key.substr(found+1);
        uint16_t instance = (uint16_t)std::stoi(stp_instance);
        Port port;

        if (!gPortsOrch->getPort(port_alias, port))
        {
            SWSS_LOG_ERROR("Failed to get port for %s alias", port_alias.c_str());
            return;
        }

        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            string stp_state;
            uint8_t state = STP_STATE_INVALID;

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "state")
                {
                    stp_state = fvValue(i);
                    state = (uint8_t)std::stoi(stp_state);
                }
            }

            if(state == STP_STATE_INVALID)
            {
                SWSS_LOG_ERROR("No stp state found for instance %u port %s", instance, port_alias.c_str());
            }
            else
            {
                if(!updateStpPortState(port, instance, state))
                {
                    it++;
                    continue;
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            if(!removeStpPort(port, instance))
            {
                it++;
                continue;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }
        it = consumer.m_toSync.erase(it);
    }
}

void StpOrch::doStpFastageTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;

        string vlan_alias = kfvKey(t);

        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            string state;
            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "state")
                    state = fvValue(i);
            }

            if(state.compare("true") == 0)
            {
                stpVlanFdbFlush(vlan_alias);
            }
        }
        else if (op == DEL_COMMAND)
        {
            //no op
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }
        it = consumer.m_toSync.erase(it);
    }
}

void StpOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    string table_name = consumer.getTableName();
    if (table_name == APP_STP_VLAN_INSTANCE_TABLE_NAME)
    {
        doStpTask(consumer);
    }
    else if (table_name == APP_STP_PORT_STATE_TABLE_NAME)
    {
        doStpPortStateTask(consumer);
    }
    else if (table_name == APP_STP_FASTAGEING_FLUSH_TABLE_NAME)
    {
        doStpFastageTask(consumer);
    }
}

