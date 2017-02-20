/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "router_core_private.h"
#include "module.h"

/**
 * Creates a thread that is dedicated to managing and using the routing table.
 * The purpose of moving this function into one thread is to remove the widespread
 * lock contention that happens with synchrounous multi-threaded routing.
 *
 * This module owns, manages, and uses the router-link list and the address hash table
 */

typedef struct qdrc_module_t {
    DEQ_LINKS(struct qdrc_module_t);
    const char          *name;
    qdrc_module_enable_t enable;
    qdrc_module_init_t   on_init;
    qdrc_module_final_t  on_final;
    void                *context;
    bool                 enabled;
} qdrc_module_t;

DEQ_DECLARE(qdrc_module_t, qdrc_module_list_t);
static qdrc_module_list_t registered_modules = {0,0};

void qdr_register_core_module(const char *name, qdrc_module_enable_t enable, qdrc_module_init_t on_init, qdrc_module_final_t on_final)
{
    qdrc_module_t *module = NEW(qdrc_module_t);
    ZERO(module);
    module->name     = name;
    module->enable   = enable;
    module->on_init  = on_init;
    module->on_final = on_final;
    DEQ_INSERT_TAIL(registered_modules, module);
}


static void qdr_activate_connections_CT(qdr_core_t *core)
{
    qdr_connection_t *conn = DEQ_HEAD(core->connections_to_activate);
    while (conn) {
        DEQ_REMOVE_HEAD_N(ACTIVATE, core->connections_to_activate);
        conn->in_activate_list = false;
        qd_server_activate((qd_connection_t*) qdr_connection_get_context(conn));
        conn = DEQ_HEAD(core->connections_to_activate);
    }
}

static inline bool try_execute(qdr_core_t *const core, bool is_running) {
    uint8_t *msg_claim = NULL;
    uint64_t claimed_position;
    if (!try_fs_rb_claim_read(&core->action_list, &claimed_position, &msg_claim)) {
        //according to claim read semantic it means that the q is empty
        return false;
    }
    qdr_action_t *action = (qdr_action_t *) msg_claim;
    if (action->label) {
        qd_log(core->log, QD_LOG_TRACE, "Core action '%s'%s", action->label, is_running ? "" : " (discard)");
    }
    action->action_handler(core, action, !is_running);
    //from now we can commit the read claim
    fs_rb_commit_read(&core->action_list, claimed_position, msg_claim);
    return true;
}

static void qdr_do_message_to_addr_free(qdr_core_t *core, qdr_general_work_t *work)
{
    qdr_delivery_cleanup_t *cleanup = DEQ_HEAD(work->delivery_cleanup_list);

    while (cleanup) {
        DEQ_REMOVE_HEAD(work->delivery_cleanup_list);
        if (cleanup->msg)
            qd_message_free(cleanup->msg);
        if (cleanup->iter)
            qd_iterator_free(cleanup->iter);
        free_qdr_delivery_cleanup_t(cleanup);
        cleanup = DEQ_HEAD(work->delivery_cleanup_list);
    }
}


void qdr_modules_init(qdr_core_t *core)
{
    //
    // Initialize registered modules
    //
    qdrc_module_t *module = DEQ_HEAD(registered_modules);
    while (module) {
        module->enabled = module->enable(core);
        if (module->enabled) {
            module->on_init(core, &module->context);
            qd_log(core->log, QD_LOG_INFO, "Core module enabled: %s", module->name);
        } else
            qd_log(core->log, QD_LOG_INFO, "Core module present but disabled: %s", module->name);

        module = DEQ_NEXT(module);
    }
}


void qdr_modules_finalize(qdr_core_t *core)
{
    //
    // Finalize registered modules
    //
    qdrc_module_t *module = DEQ_TAIL(registered_modules);
    while (module) {
        if (module->enabled) {
            qd_log(core->log, QD_LOG_INFO, "Finalizing core module: %s", module->name);
            module->on_final(module->context);
        }
        module = DEQ_PREV(module);
    }

}


void *router_core_thread(void *arg)
{
    qdr_core_t        *core = (qdr_core_t*) arg;

    qdr_forwarder_setup_CT(core);
    qdr_route_table_setup_CT(core);
    qdr_agent_setup_CT(core);
    //TODO this heuristic is assuming fairness between workers
    const int max_batch_size = core->qd->max_batch_size;
    qd_log(core->log, QD_LOG_INFO, "Router Core thread running. %s/%s", core->router_area, core->router_id);
    qd_log(core->log, QD_LOG_INFO, "Using max_batch_size = %d", max_batch_size);
    qdr_modules_init(core);
    atomic_store_explicit(&core->core_status.sleeping, false, memory_order_seq_cst);
    bool is_running;
    while ((is_running = atomic_load_explicit(&core->running, memory_order_acquire)) ||
           !fs_rb_is_empty(&core->action_list)) {

        int read_batch = 0;
        bool is_empty = true;
        while (try_execute(core, is_running)) {
            read_batch++;
            if (read_batch == max_batch_size) {
                is_empty = false;
                break;
            }
        }
        //
        // Activate all connections that were flagged for activation during the above processing
        //
        qdr_activate_connections_CT(core);

        //
        // Schedule the cleanup of deliveries freed during this core-thread pass
        //
        if (DEQ_SIZE(core->delivery_cleanup_list) > 0) {
            qdr_general_work_t work = qdr_general_work(qdr_do_message_to_addr_free);
            DEQ_MOVE(core->delivery_cleanup_list, work.delivery_cleanup_list);
            qdr_post_general_work_CT(core, &work);
        }
        //This cascade of isEmpty checks must be preserved to save sleeping if possible
        if (is_empty && fs_rb_is_empty(&core->action_list)) {
            //we need a full barrier to avoid fs_rb_is_empty() to move before this
            atomic_store_explicit(&core->core_status.sleeping, true, memory_order_seq_cst);
            if (fs_rb_is_empty(&core->action_list)) {
                //save going to sleep if there is a signal in progress
                if (!atomic_load_explicit(&core->core_status.wakeup.signaling, memory_order_acquire)) {
                    sys_mutex_lock(core->core_status.wakeup.lock);
                    //no need to wait if a racing producer has submitted something to do
                    if (fs_rb_is_empty(&core->action_list)) {
                        sys_cond_wait(core->core_status.wakeup.cond, core->core_status.wakeup.lock);
                    }
                    sys_mutex_unlock(core->core_status.wakeup.lock);
                }
            }
            atomic_store_explicit(&core->core_status.sleeping, false, memory_order_release);
        }
    }
    const uint32_t remaining_actions = fs_rb_size(&core->action_list);
    if (remaining_actions > 0) {
        qd_log(core->log, QD_LOG_INFO, "Router Core thread exited leaving %d actions yet to be processed",
               remaining_actions);
    } else {
        qd_log(core->log, QD_LOG_INFO, "Router Core thread exited");
    }
    return 0;
}
