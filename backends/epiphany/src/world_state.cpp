#include "world_state.hpp"
extern "C" {
#include <e-lib.h>
}
#include <bulk/variable.hpp>
#include <bulk/messages.hpp>

namespace bulk {
namespace epiphany {

// The global instance of world_state
world_state state;

void init_dma_handlers();

EXT_MEM_TEXT world_state::world_state() {
    int row = e_group_config.core_row;
    int col = e_group_config.core_col;
    int cols = e_group_config.group_cols;
    int rows = e_group_config.group_rows;

    // The global instance of world is located in the .bss section which
    // means that it will be initialized to zero by the program loader.
    // Therefore we only initialize non-zero member variables here.

    // Initialize nonzero local data
    local_pid_ = col + cols * row;
    nprocs_ = cols * rows;

    int s = 0;
    for (int i = 0; i < rows; i++)
        for (int j = 0; j < cols; j++)
            coreids_[s++] = (uint16_t)e_coreid_from_coords(i, j);

    barrier_init_();

    // Send &syncstate to ARM
    if (local_pid_ == 0)
        combuf_->syncstate_ptr = (int8_t*)&syncstate_;

    init_dma_handlers();

    write_syncstate_(SYNCSTATE::RUN);
    sync();
}

world_state::~world_state() { write_syncstate_(SYNCSTATE::FINISH); }

// Most of sync tasks are done in world::sync !
void world_state::sync() {
    // var puts
    for (var_id_t i = 0; i < MAX_VARS; ++i) {
        auto& v = var_list_[i];
        if (v.base != 0) {
            if (v.do_put) {
                // we have written something to receiveBuffer earlier
                // now copy it to the var itself
                v.base->deserialize_put(v.size, v.receiveBuffer);
            }
            v.do_put = 0;
        }
    }

    // Queue messages to this processor
    for (var_id_t i = 0; i < MAX_VARS; ++i) {
        auto& rq = queue_list_[i];
        if (rq.base != 0) { // If this is a registered queue
            rq.base->clear_();
            for (auto& p : rq.receiveBuffers) {
                rq.base->deserialize_push(p.second, p.first);
                delete[] p.first;
            }
            rq.receiveBuffers.clear();
        }
    }
}

void world_state::barrier() {
    if (local_pid_ == 0) {
        // Flip pass
        // set "my" slot
        sync_barrier_[local_pid_] = 1;
        // poll on all slots
        for (int i = 1; i < nprocs_; i++)
            while (sync_barrier_[i] == 0) {
            };

        // Flop pass
        // clear all local slots
        for (int i = 0; i < nprocs_; i++)
            sync_barrier_[i] = 0;
        // set remote slots
        for (int i = 1; i < nprocs_; i++)
            *(sync_barrier_tgt_[i]) = 1;
    } else {
        // Flip pass
        // set "my" remote slot
        *(sync_barrier_tgt_[0]) = 1;

        // Flop pass
        // poll on "my" local slot
        while (sync_barrier_[0] == 0) {
        };
        // clear "my" local slot
        sync_barrier_[0] = 0;
    }
}

void world_state::barrier_init_() {
    if (local_pid_ == 0) {
        for (int s = 0; s < nprocs_; s++)
            sync_barrier_tgt_[s] = (volatile int8_t*)transform_address_local_(
                (void*)&sync_barrier_[0], s);
    } else {
        sync_barrier_tgt_[0] = (volatile int8_t*)transform_address_local_(
            (void*)&sync_barrier_[local_pid_], 0);
    }
}
} // namespace epiphany
} // namespace bulk

