#include <errno.h>
#include <pthread.h>
#include <sensors/sensors.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termcap.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CPUINFO_PATH "/proc/cpuinfo"
#define PROCSTAT_PATH "/proc/stat"
#define CPU_PRESENT_PATH "/sys/devices/system/cpu/present"
#define CPU_ONLINE_PATH "/sys/devices/system/cpu/online"
#define CPU_FREQ_PATTERN "/sys/devices/system/cpu/cpu%u/cpufreq/%s"
#define CPU_TOPOLOGY_PATTERN "/sys/devices/system/cpu/cpu%u/topology/%s"
#define READ_MODE "r"

#define BOX_DRAWING_BEG "\x1b(0"
#define BOX_DRAWING_END "\x1b(B"
#define VRT BOX_DRAWING_BEG"\x78"BOX_DRAWING_END

#define NORMAL_COLOR "\x1B[0m"
#define GREEN  "\x1B[32m"
#define BLUE  "\x1B[34m"

pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER;

void sig_handler(int signum) {
    pthread_cond_broadcast(&cond);
}

// Checks if a string starts with the specified prefix
bool startswith(const char* str, const char* prefix) {
    while (*str != '\0') {
        if (*prefix == '\0') {
            return true;
        }
        if (*str++ != *prefix++) {
            return false;
        }
    }
    return false;
}

void log_err(const char* format, ...) {
    va_list args;
    va_start(args, format);

    char msg[1024];
    vsnprintf(msg, sizeof(msg)/sizeof(char), format, args);

    va_end(args);

    if (errno == 0) {
        fprintf(stderr, "%s.\n", msg);
    } else {
        char* err_msg = strerror(errno);
        fprintf(stderr, "%s: %s.\n", msg, err_msg);
    }
}

void add_nsec(struct timespec* ts, int64_t nsec) {
    // a valid nsec is [0; 999999999]
    // so it should be normalize
    nsec += ts->tv_nsec;
    if (nsec > 999999999) {
        ts->tv_sec += nsec / 1000000000;
        ts->tv_nsec = nsec % 1000000000;
    } else {
        ts->tv_nsec = nsec;
    }
}

// Block the current thread for the given nanoseconds
// On success returns 0, if the specified duration has passed returns 1
// if an error occurs returns -1
int wait_for(pthread_cond_t* var, pthread_mutex_t* m, int64_t nsec) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    add_nsec(&ts, nsec);

    int res = pthread_cond_timedwait(var, m, &ts);
    if (res == 0) {
        return 1;
    } else if (res == ETIMEDOUT) {
        return 0;
    }

    log_err("Failed to wait on condition variable: %s", strerror(res));
    return -1;
}

void* process_key_press(void* data) {
    struct termios orig_term_attr;
    struct termios new_term_attr;

    // store origin attributes
    tcgetattr(fileno(stdin), &orig_term_attr);

    // set attributes for non-blocking stdin
    memcpy(&new_term_attr, &orig_term_attr, sizeof(struct termios));
    new_term_attr.c_lflag &= ~(ECHO | ICANON);
    new_term_attr.c_cc[VTIME] = 0;
    new_term_attr.c_cc[VMIN] = 0;

    tcsetattr(fileno(stdin), TCSANOW, &new_term_attr);

    while (true) {
        pthread_mutex_lock(&mutex1);

        char ch = '\0';
        if (read(fileno(stdin), &ch, 1) > 0) {
            // Quite on q
            if (ch == 'q') {
                break;
            }
        }

        // wait for 100ms
        if (wait_for(&cond, &mutex1, 100000000) != 0) {
            break;
        }

        pthread_mutex_unlock(&mutex1);
    }
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex1);

    // restore origin attributes
    tcsetattr(fileno(stdin), TCSANOW, &orig_term_attr);

    return NULL;
}

struct thread_usage {
    int total;
    int active;
    int usage;
};

struct thread_info {
    int core_id;
    float scaling_cur_freq;
    struct thread_usage usage;
    double temp;
    int online;
};

// increase array size twice if idx is bigger than current size
void increase_size(struct thread_info** threads, int idx, int* size) {
    if (idx >= *size) {
        int oldsize = *size;
        *size *= 2;
        if (idx >= *size) {
            *size = idx + 1;
        }
        printf("Increase size\n");
        *threads = realloc(*threads, sizeof(struct thread_info) * (*size));
        memset(*threads + oldsize, 0, sizeof(struct thread_info) * (*size - oldsize));
    }
}

// Reads from the file the possible number of cpus and allocate memory
// On success returns 1 otherwise -1
int init_cpus(struct thread_info** cpus, int* size) {
    FILE* fp = fopen(CPU_PRESENT_PATH, READ_MODE);
    if (fp == NULL) {
        log_err("Failed to open %s", CPU_PRESENT_PATH);
        return -1;
    }

    char* line = NULL;
    size_t len = 0;
    if (getline(&line, &len, fp) == -1) {
        log_err("Failed to read available cpus from %s", CPU_PRESENT_PATH);
        return -1;
    }

    char* errmsg = "Failed to parse possible cpus. "
                   "Expected format: %d-%d or 1";

    int beg = 0, end = 0;
    if (sscanf(line, "%d-%d", &beg, &end) == 2) {
        *size = end - beg + 1;
    } else if (sscanf(line, "%d", size) == 1) {
        if (*size != 1) {
            log_err(errmsg);
            return -1;
        }
    } else {
        log_err(errmsg);
        return -1;
    }

    *cpus = calloc(*size, sizeof(struct thread_info));
    if (*cpus == NULL) {
        log_err("Failed to allocate memory for cpus");
        return -1;
    }

    free(line);
    fclose(fp);
    return 1;
}

int set_online(struct thread_info* cpus, int size) {
    FILE* fp = fopen(CPU_ONLINE_PATH, READ_MODE);
    if (fp == NULL) {
        log_err("Failed to open %s", CPU_ONLINE_PATH);
        return -1;
    }

    for (int idx = 0; idx < size; ++idx) {
      cpus[idx].online = 0;
    }

    char* line = NULL;
    size_t len = 0;
    char delim = ',';

    ssize_t sz = 0;
    while ((sz = getdelim(&line, &len, delim, fp)) != -1) {
        int beg = 0, end = 0;
        if (sscanf(line, "%d-%d", &beg, &end) == 2) {
          for (;beg <= end; ++beg) {
              cpus[beg].online = 1;
          }
        } else if (sscanf(line, "%d", &beg) == 1) {
          cpus[beg].online = 1;
        }
    }
    free(line);
    fclose(fp);
    return 1;
}

int read_value_from_file(const char* path, int* value) {
    FILE* fp = fopen(path, READ_MODE);
    if (fp == NULL) {
        log_err("Failed to open the file %s", path);
        return -1;
    }

    char* line = NULL;
    size_t len = 0;
    if (getline(&line, &len, fp) == -1) {
        free(line);
        log_err("Failed to read value from file");
        return -1;
    }

    *value = atoi(line);
    free(line);
    fclose(fp);

    return 1;
}

int read_thread_info(struct thread_info* threads, int size) {
    for (int idx = 0; idx < size; ++idx) {
        if (!threads[idx].online) {
          continue;
        }
        char path[128];

        sprintf(path, CPU_FREQ_PATTERN, idx, "scaling_cur_freq");
        int scaling_cur_freq = 0;
        if (read_value_from_file(path, &scaling_cur_freq) == -1) {
            return -1;
        }
        threads[idx].scaling_cur_freq = 1.0 * scaling_cur_freq / 1000.0;

        sprintf(path, CPU_TOPOLOGY_PATTERN, idx, "core_id");
        if (read_value_from_file(path, &(threads[idx].core_id)) == -1) {
            return -1;
        }
    }
    return 1;
}

// Returns 1 on success otherwise -1
int read_model_name(char* model_name) {
    FILE* fp = NULL;
    if ((fp = fopen(CPUINFO_PATH, READ_MODE)) == NULL) {
        log_err("Failed to open %s", CPUINFO_PATH);
        return -1;
    }

    char* line = NULL;
    size_t len = 0;
    ssize_t nread = 0;

    int ret = -1;
    while ((nread = getline(&line, &len, fp)) != -1) {
        const char* prefix = "model name\t: ";
        size_t sz = 13;
        if (startswith(line, prefix)) {
            memcpy(model_name, line + sz, nread - sz);
            model_name[nread-sz-1] = '\0';
            ret = 1;
            break;
        }
    }

    free(line);
    return ret;
}

int read_thread_usage(struct thread_info** threads, int* size) {
    FILE* fp = NULL;
    if ((fp = fopen(PROCSTAT_PATH, READ_MODE)) == NULL) {
        log_err("Failed to open %s", PROCSTAT_PATH);
        return -1;
    }

    if (*size == 0) {
        *threads = (struct thread_info*)calloc(1, sizeof(struct thread_info));
        *size = 1;
    }

    char* line = NULL;
    size_t len = 0;
    ssize_t nread = 0;

    while ((nread = getline(&line, &len, fp)) != -1) {
        int id = 0, user = 0, nice = 0, system = 0, idle = 0, iowait = 0,
            irq = 0, softirq = 0, steal = 0, guest = 0, guestnice = 0;
        if (sscanf(line, "cpu%d %d %d %d %d %d %d %d %d %d %d",
                   &id, &user, &nice, &system, &idle, &iowait, &irq,
                   &softirq, &steal, &guest, &guestnice) == 11) {
            idle += iowait;
            int total = user + nice + system + idle + irq + softirq
                             + steal + guest + guestnice;
            int active = total - idle;

            increase_size(threads, id, size);

            int dtotal = total - (*threads)[id].usage.total;
            int dactive = active - (*threads)[id].usage.active;
            if (dtotal != 0) {
                (*threads)[id].usage.usage = ((dactive * 1.0) / (dtotal * 1.0) * 100.0);
            }

            (*threads)[id].usage.total = total;
            (*threads)[id].usage.active = active;
        }
    }
    fclose(fp);
    return 1;
}

int read_cpu_temp(struct thread_info* threads, int size) {
    const sensors_chip_name* cn = NULL;
    int c = 0;
    while ((cn = sensors_get_detected_chips(NULL, &c))) {
        // For Intel processors
        if (strcmp(cn->prefix, "coretemp") == 0) {
            const sensors_feature* feat;
            int f = 0;
            while ((feat = sensors_get_features(cn, &f)) != 0) {
                char * label = sensors_get_label(cn, feat);
                int core_id = 0;
                if (!label || sscanf(label, "Core %d", &core_id) != 1) {
                    continue;
                }

                const sensors_subfeature* temp_input =
                  sensors_get_subfeature(cn, feat, SENSORS_SUBFEATURE_TEMP_INPUT);
                if (temp_input) {
                    double val = 0.0;
                    if (sensors_get_value(cn, temp_input->number, &val) == 0) {
                        for (int j = 0; j < size; ++j) {
                            if (threads[j].core_id == core_id) {
                                threads[j].temp = val;
                            }
                        }
                    }
                }
            }
            break;
        }
        // For AMD processors
        else if (strcmp(cn->prefix, "k10temp") == 0) {
            const sensors_feature* feat;
            int f = 0;
            while ((feat = sensors_get_features(cn, &f)) != 0) {
                char * label = sensors_get_label(cn, feat);
                if (strcmp(label, "Tctl") == 0) {
                    const sensors_subfeature* temp_input =
                      sensors_get_subfeature(cn, feat, SENSORS_SUBFEATURE_TEMP_INPUT);
                    if (temp_input) {
                        double val = 0.0;
                        if (sensors_get_value(cn, temp_input->number, &val) == 0) {
                            for (int j = 0; j < size; ++j) {
                                threads[j].temp = val;
                            }
                        }
                    }
                }
            }
            break;
        }
        // ARMv7 Processor
        else if (strcmp(cn->prefix, "cpu_thermal") == 0) {
            const sensors_feature* feat;
            int f = 0;
            while ((feat = sensors_get_features(cn, &f)) != 0) {
                char * label = sensors_get_label(cn, feat);
                if (strcmp(label, "temp1") == 0) {
                    const sensors_subfeature* temp_input =
                      sensors_get_subfeature(cn, feat, SENSORS_SUBFEATURE_TEMP_INPUT);
                    if (temp_input) {
                        double val = 0.0;
                        if (sensors_get_value(cn, temp_input->number, &val) == 0) {
                            for (int j = 0; j < size; ++j) {
                                threads[j].temp = val;
                            }
                        }
                    }
                }
            }
            break;
        }
    }
}

// Draw line with the given chars
void draw_line(const int* width, int size, char beg, char end, char delim, char fill) {
    printf("%s", BOX_DRAWING_BEG);
    putchar(beg);
    for (int idx = 0; idx < size; ++idx) {
        for (int j = 0; j < width[idx]; ++j) {
            putchar(fill);
        }
        if (idx != size - 1) {
            putchar(delim);
        }
    }
    putchar(end);
    printf("%s", BOX_DRAWING_END);
}

void draw_top_line(const int* width, int size) {
    draw_line(width, size, 0x6c, 0x6b, 0x77, 0x71);
}

void draw_middle_line(const int* width, int size) {
    draw_line(width, size, 0x74, 0x75, 0x6e, 0x71);
}

void draw_bottom_line(const int* width, int size) {
    draw_line(width, size, 0x6d, 0x6a, 0x76, 0x71);
}

void print_thread_info(const struct thread_info* threads, int size) {
    for (size_t idx = 0; idx < size; ++idx) {
        if (threads[idx].online) {
        printf("%s %6i %s %4i %s %6i %s %10.3f %s %8.1f %s %4d%% %s\n",
                VRT, idx, VRT, threads[idx].core_id, VRT,
                threads[idx].online, VRT,
                threads[idx].scaling_cur_freq, VRT,
                threads[idx].temp, VRT,
                threads[idx].usage.usage, VRT);
        } else {
        printf("%s %6i %s %4i %s %6i %s %10.3f %s %8.1f %s %4d%% %s\n",
                VRT, idx, VRT, threads[idx].core_id, VRT,
                threads[idx].online, VRT,
                0.0, VRT,
                0.0, VRT,
                0, VRT);
        }
    }
}

static char
    *sc_move,
    *sc_init,
    *sc_deinit,
    *sc_cursor_invisible,
    *sc_cursor_normal;

// move cursor into the given position
void move_cursor(const char* command, int hpos, int vpos) {
    if (command != NULL) {
        char* go = tgoto(command, hpos, vpos);
        if (go != NULL) {
            tputs(go, 1, putchar);
        }
    }
}

void move_cursor_up(int lines) {
    printf("\033[%dA", lines);
}

void move_cursor_backward(int columns) {
    printf("\033[%dD", columns);
}

int main(int argc, char* argv[])
{
    bool fullscreen_mode = false;
    for (int idx = 1; idx < argc; ++idx) {
        if (strcmp(argv[idx], "--fullscreen") == 0) {
            fullscreen_mode = true;
        }
    }

    char *termtype = getenv("TERM");
    if (termtype == NULL) {
        termtype = "unknown";
    }
    static char termbuf[2048];
    tgetent(termbuf, termtype);

    // init strings of commands
    sc_init = tgetstr("ti", NULL);
    sc_deinit = tgetstr("te", NULL);
    sc_move = tgetstr("cm", NULL);
    sc_cursor_invisible = tgetstr("vi", NULL);
    sc_cursor_normal = tgetstr("ve", NULL);

    if (fullscreen_mode && sc_init == NULL) {
        fullscreen_mode = false;
    }

    // set custom signal handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_handler = sig_handler;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // init sensors lib
    bool sensors = true;
    if (sensors_init(NULL) != 0) {
        sensors = false;
    }

    // run second thread to process key presses
    pthread_t th;
    int ret = pthread_create(&th, NULL, &process_key_press, NULL);
    if (ret != 0) {
        log_err("Failed to create a thread: %s", strerror(ret));
        return 1;
    }

    struct thread_info* threads = NULL;
    int thread_count = 0;
    if (init_cpus(&threads, &thread_count) == -1) {
        log_err("Error during initialization");
        return 1;
    }

    // init fullscreen mode
    if (fullscreen_mode) {
        fputs(sc_init, stdout);
        move_cursor(sc_move, 0, 0);
    }

    char model_name[128];
    int is_read_model_name = read_model_name(model_name);
    if (is_read_model_name == 1) {
        printf("Processor: %s%s%s\n", BLUE, model_name, NORMAL_COLOR);
    }

    // print headers
    int width[] = {8, 6, 8, 12, 10, 7};
    size_t width_sz = sizeof(width) / sizeof(int);
    draw_top_line(width, width_sz);
    printf("\n%s %6s %s %4s %s %6s %s %10s %s %8s %s %5s %s\n",
            VRT, "Thread", VRT, "Core",
            VRT, "Online",
            VRT, "Speed, MHz", VRT, "Temp, °C" , VRT, "Usage", VRT);
    draw_middle_line(width, width_sz);
    printf("\n");

    // set invisible cursor
    tputs(sc_cursor_invisible, 1, putchar);

    bool break_on_error = false;

    pthread_mutex_lock(&mutex1);
    while (true) {
        if (-1 == set_online(threads, thread_count)) {
            break_on_error = true;
            break;
        }

        if (-1 == read_thread_info(threads, thread_count)) {
            break_on_error = true;
            break;
        }

        if (-1 == read_thread_usage(&threads, &thread_count)) {
            break_on_error = true;
            break;
        }

        if (sensors) {
            if (-1 == read_cpu_temp(threads, thread_count)) {
                break_on_error = true;
                break;
            }
        }

        print_thread_info(threads, thread_count);
        draw_bottom_line(width, width_sz);
        fflush(stdout);

        // wait for 2 second
        if (wait_for(&cond, &mutex1, 2000000000) != 0) {
            break;
        }

        // move cursor back
        if (fullscreen_mode) {
            move_cursor(sc_move, 0, 4);
        } else {
            move_cursor_up(thread_count);
            move_cursor_backward(58);
        }
    }
    pthread_mutex_unlock(&mutex1);

    if (break_on_error) {
        pthread_cond_signal(&cond);
    }

    // set cursor normal mode back
    tputs(sc_cursor_normal, 1, putchar);

    pthread_join(th, NULL);
    pthread_cond_destroy(&cond);
    pthread_mutex_destroy(&mutex1);

    if (sensors) {
        sensors_cleanup();
    }

    // exit fullscreen mode
    if (fullscreen_mode && sc_deinit != NULL) {
        fputs(sc_deinit, stdout);
    }

    free(threads);

    if (!fullscreen_mode) {
        printf("\n");
    }
    return 0;
}
