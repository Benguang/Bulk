#include <environment_provider.hpp>

// #define __USE_XOPEN2K // already defined in one of the C++ headers
#include <unistd.h> // For `access` and `readlink`
// #define __USE_XOPEN2K
#define __USE_POSIX199309 1
#include <time.h> // clock_gettime and clock_nanosleep

namespace bulk {
namespace epiphany {

void provider::spawn(int processors, const char* image_name) {
    if (!is_valid()) {
        std::cerr << "ERROR: spawn called on hub that was not properly "
                     "initialized."
                  << std::endl;
        return;
    }

    nprocs_used_ = processors;

    if (processors < 1 || processors > NPROCS) {
        std::cerr << "ERROR: spawn called with processors = " << processors
                  << std::endl;
    }

    e_fullpath_ = e_directory_;
    e_fullpath_.append(image_name);

    // Check if the file exists
    if (access(e_fullpath_.c_str(), R_OK) == -1) {
        std::cerr << "ERROR: Could not find epiphany executable: "
                  << e_fullpath_ << std::endl;
        return;
    }

    if (e_load_group(e_fullpath_.c_str(), &dev_, 0, 0, rows_, cols_, E_FALSE) !=
        E_OK) {
        std::cerr << "ERROR: Could not load program to chip." << std::endl;
        return;
    }

    combuf_->nprocs = nprocs_used_;
    for (int i = 0; i < NPROCS; i++)
        combuf_->syncstate[i] = SYNCSTATE::INIT;

    // Starting time
    clock_gettime(CLOCK_MONOTONIC, &ts_start_);
    update_remote_timer_();

    // Start the program by sending SYNC interrupts
    if (e_start_group(&dev_) != E_OK) {
        std::cerr << "ERROR: e_start_group() failed." << std::endl;
        return;
    }

    // Main program loop
    int extmem_corrupted = 0;
    for (;;) {
        update_remote_timer_();
        microsleep_(1); // 1000 is 1 millisecond

        // Check sync states
        int counters[SYNCSTATE::COUNT] = {0};

        for (int i = 0; i < NPROCS; i++) {
            SYNCSTATE s = (SYNCSTATE)combuf_->syncstate[i];
            if (s >= 0 && s < SYNCSTATE::COUNT) {
                counters[s]++;
            } else {
                extmem_corrupted++;
                if (extmem_corrupted <= 32) { // to avoid overflow
                    std::cerr << "ERROR: External memory corrupted.";
                    std::cerr << " syncstate[" << i << "] = " << s << std::endl;
                }
            }

            if (s == SYNCSTATE::MESSAGE) {
                printf("$%02d: %s\n", i, combuf_->msgbuf);
                fflush(stdout);
                // Reset flag to let epiphany core continue
                set_core_syncstate_(i, SYNCSTATE::CONTINUE);
            }
        }

        if (counters[SYNCSTATE::SYNC] == nprocs_used_) {
            std::cout << "(BSP) Host sync. Not implemented." << std::endl;
            for (int i = 0; i < nprocs_used_; i++)
                set_core_syncstate_(i, SYNCSTATE::CONTINUE);
        }

        if (counters[SYNCSTATE::ABORT]) {
            std::cout << "(BSP) ERROR: spmd program aborted." << std::endl;
            break;
        }
        if (counters[SYNCSTATE::FINISH] == nprocs_used_)
            break;
    }

    env_initialized_ = 3;
}

void provider::initialize_() {
    init_application_path_();

    // Initialize the Epiphany system
    if (e_init(NULL) != E_OK) {
        std::cerr << "ERROR: Could not initialize HAL data structures.\n";
        return;
    }

    // Reset the Epiphany system
    if (e_reset_system() != E_OK) {
        std::cerr << "ERROR: Could not reset the Epiphany system.\n";
        return;
    }

    // Get information on the platform
    if (e_get_platform_info(&platform_) != E_OK) {
        std::cerr << "ERROR: Could not obtain platform information.\n";
        return;
    }

    // Obtain the number of processors from the platform information
    rows_ = platform_.rows;
    cols_ = platform_.cols;
    nprocs_available_ = rows_ * cols_;

    env_initialized_ = 1;

    // Open the workgroup
    if (e_open(&dev_, 0, 0, rows_, cols_) != E_OK) {
        std::cerr << "ERROR: Could not open workgroup.\n";
        return;
    }

    if (e_reset_group(&dev_) != E_OK) {
        std::cerr << "ERROR: Could not reset workgroup.\n";
        return;
    }

    // e_alloc will mmap combuf and dynmem
    // The offset in external memory is equal to NEWLIB_SIZE
    if (e_alloc(&emem_, NEWLIB_SIZE, COMBUF_SIZE + DYNMEM_SIZE) != E_OK) {
        std::cerr << "ERROR: e_alloc failed.\n";
        return;
    }
    combuf_ = (combuf*)emem_.base;

    env_initialized_ = 2;
}

void provider::finalize_() {
    if (env_initialized_ >= 2)
        e_free(&emem_);

    if (env_initialized_ >= 1) {
        if (E_OK != e_finalize()) {
            std::cerr << "ERROR: Could not finalize the Epiphany connection."
                      << std::endl;
        }
    }

    env_initialized_ = 0;
}

void provider::set_core_syncstate_(int pid, SYNCSTATE state) {
    // First write it to extmem
    combuf_->syncstate[pid] = SYNCSTATE::CONTINUE;

    // Then write it to the core itself
    off_t dst = (off_t)combuf_->syncstate_ptr;
    if (e_write(&dev_, (pid / cols_), (pid % cols_), dst, &state,
                sizeof(int8_t)) != sizeof(int8_t)) {
        std::cerr << "ERROR: unable to write syncstate to core memory."
                  << std::endl;
    }
}

// Get the directory that the application is running in
// and store it in e_directory_ including the trailing slash
void provider::init_application_path_() {
    e_directory_.clear();
    char path[1024];
    ssize_t len = readlink("/proc/self/exe", path, 1024);
    if (len > 0 && len < 1024) {
        path[len] = 0;
        char* slash = strrchr(path, '/');
        if (slash) {
            *(slash + 1) = 0;
            e_directory_ = path;
        }
    }
    if (e_directory_.empty()) {
        std::cerr << "ERROR: Could not find process directory.\n";
        e_directory_ = "./";
    }
    return;
}

void provider::update_remote_timer_() {
    clock_gettime(CLOCK_MONOTONIC, &ts_end_);

    float time_elapsed = (ts_end_.tv_sec - ts_start_.tv_sec +
                          (ts_end_.tv_nsec - ts_start_.tv_nsec) * 1.0e-9);

    combuf_->remotetimer = time_elapsed;
}

void provider::microsleep_(int microseconds) {
    struct timespec request, remain;
    request.tv_sec = (int)(microseconds / 1000000);
    request.tv_nsec = (microseconds - 1000000 * request.tv_sec) * 1000;
    if (clock_nanosleep(CLOCK_MONOTONIC, 0, &request, &remain) != 0)
        std::cerr << "ERROR: clock_nanosleep was interrupted." << std::endl;
}
} // namespace epiphany
} // namespace bulk
