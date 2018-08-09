/*
 * Copyright (c) 2016 Shanghai Jiao Tong University.
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://ipads.se.sjtu.edu.cn/projects/wukong
 *
 */

#pragma once

#include "config.hpp"
#include "query.hpp"
#include "tcp_adaptor.hpp"
#include "rdma_adaptor.hpp"
#include <cstring>
#include <climits>

/// TODO: define adaptor as a C++ interface and make tcp and rdma implement it
class Adaptor {
public:
    int tid; // thread id

    TCP_Adaptor *tcp;   // communicaiton by TCP/IP
    RDMA_Adaptor *rdma; // communicaiton by RDMA

    Adaptor(int tid, TCP_Adaptor *tcp, RDMA_Adaptor *rdma)
        : tid(tid), tcp(tcp), rdma(rdma) { }

    ~Adaptor() { }

    bool send(int dst_sid, int dst_tid, const string &str) {
        if (global_use_rdma && rdma->init)
            return rdma->send(tid, dst_sid, dst_tid, str);
        else
            return tcp->send(dst_sid, dst_tid, str);
    }

    bool send(int dst_sid, int dst_tid, const Bundle &b) {
        string str = b.to_str();
        return send(dst_sid, dst_tid, str);
    }

    string recv() {
        if (global_use_rdma && rdma->init)
            return rdma->recv(tid);
        else
            return tcp->recv(tid);
    }

    void recv(Bundle &b) {
        string str = recv();
        b.init(str.c_str(), str.length());
    }

    bool tryrecv(string &str) {
        if (global_use_rdma && rdma->init) {
            int dst_sid_out = 0;
            if (!rdma->tryrecv(tid, dst_sid_out, str)) return false;
        } else {
            if (!tcp->tryrecv(tid, str)) return false;
        }
        return true;
    }

    bool tryrecv(Bundle &b) {
        string str;
        if (!tryrecv(str)) return false;
        b.init(str.c_str(), str.length());
        return true;
    }

    #ifdef USE_GPU
    /* send the forked subquery(in CPU mem) and partial history(in GPU mem) to remote server
     * table_size refers to the number of elements in history, not n_rows
     */
    bool send_split(int dst_sid, int dst_tid, SPARQLQuery &r, char *history_ptr, uint64_t table_size) {
        ASSERT(tid < global_num_threads);
        ASSERT(r.subquery_type == SPARQLQuery::SubQueryType::SPLIT);
        Bundle bundle(r);
        return rdma->send_split(tid, dst_sid, dst_tid, bundle.to_str(), history_ptr, table_size * sizeof(sid_t));
    }

    bool send_device2host(int dst_sid, int dst_tid, char *history_ptr, uint64_t table_size) {
        ASSERT(tid < global_num_threads);
        return rdma->send_device2host(tid, dst_sid, dst_tid, history_ptr, table_size * sizeof(sid_t));
    }

    /* first receive the forked subquery, then receive the partial history and copy it to local gpu mem
     * receive does not need acquire lock since there are only one reader on ring buffer
     */
    bool tryrecv_split(SPARQLQuery &r) {
        string str;
        int sender_sid = 0;

        if (!rdma->tryrecv(tid, sender_sid, str))
            return false;

        Bundle b(str.c_str(), str.length());

        r = b.get_sparql_query();

        // continue receive history of query
        if (r.subquery_type == SPARQLQuery::SubQueryType::SPLIT) {
            int ret;
            string dumb_str;

            ret = rdma->recv_by_gpu(tid, sender_sid, dumb_str);
            ASSERT(ret > 0);
            GPU &gpu = GPU::instance();
            // hint: history has been copied to gpu mem(by recv_by_gpu->fetch), update r.result.gpu_history_ptr & r.result.gpu_history_table_size here
            r.result.gpu_history_ptr = gpu.history_inbuf();
            r.result.gpu_history_table_size = gpu.history_size();
        }
        return true;
    }
    #endif
};
