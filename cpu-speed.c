#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <termios.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <termcap.h>

pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER;

void sig_handler(int signum) {
    pthread_cond_broadcast(&cond);
}

// check if an string starts with a begin substring
bool startswith(const char* str, const char* substr) {
    while (*str != '\0') {
        if (*substr == '\0') {
            return true;
        }
        if (*str++ != *substr++) {
            return false;
        }
    }
    return false;
}

void add_nsec(struct timespec* ts, int64_t nsec) {
    // a valid nsec is [0; 999999999]
    // so it should be normalize
    nsec += ts->tv_nsec;
    if (nsec > 999999999) {
        ts->tv_sec += nsec / 1000000000;
        ts->tv_nsec = nsec % 1000000000;
    }
}

// block the current thread for the given nanoseconds
int wait_for(pthread_cond_t* var, pthread_mutex_t* m, int nsec) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    add_nsec(&ts, nsec);

    int res = pthread_cond_timedwait(var, m, &ts);
    if (res == 0) {
        return 1;
    } else if (res == ETIMEDOUT) {
        return 0;
    }

    fprintf(stderr, "\npthread_cond_timedwait is failed with: %i\n", res);
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
        if (read(fileno(stdin), &ch, 1 > 0)) {
            // Quite on q
            if (ch == 'q') {
                break;
            }
        }

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
    float frequency;
    struct thread_usage usage;
};

// increase array size twice if idx is bigger than current size
void increase_size(struct thread_info** threads, int idx, int* size) {
    if (idx >= *size) {
        int oldsize = *size;
        *size *= 2;
        if (idx >= *size) {
            *size = idx + 1;
        }
        *threads = realloc(*threads, sizeof(struct thread_info) * (*size));
        memset(*threads + oldsize, 0, sizeof(struct thread_info) * (*size - oldsize));
    }
}

int read_thread_info(struct thread_info** threads, int* size) {
    FILE* fp = NULL;
    char* filepath = "/proc/cpuinfo";
    if ((fp = fopen(filepath, "r")) == NULL) {
        fprintf(stderr, "Failed to open %s", filepath);
        return -1;
    }

    if (*size == 0) {
        *threads = (struct thread_info*)calloc(0, sizeof(struct thread_info));
        *size = 1;
    }

    char* line = NULL;
    size_t len = 0;
    ssize_t nread = 0;

    int id = 0;
    while ((nread = getline(&line, &len, fp)) != -1) {
        if (startswith(line, "processor\t:")) {
            id = atoi(line + 11);
            increase_size(threads, id, size);
        }
        else if (startswith(line, "cpu MHz\t\t:")) { 
            (*threads)[id].frequency = atof(line + 10);
        }
        else if (startswith(line, "core id\t\t:")) {
            (*threads)[id].core_id = atoi(line + 10);
        }
    }
    fclose(fp);
    return 1;
}

int read_thread_usage(struct thread_info** threads, int* size) {
    FILE* fp = NULL;
    char* filepath = "/proc/stat";
    if ((fp = fopen(filepath, "r")) == NULL) {
        fprintf(stderr, "Failed to open %s", filepath);
        return -1;
    }

    if (*size == 0) {
        *threads = (struct thread_info*)calloc(0, sizeof(struct thread_info));
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

void print_thread_info(const struct thread_info* threads, int size) {
    for (size_t idx = 0; idx < size; ++idx) {
        printf("| %6i | %4i | %10.3f | %4d% |\n",
                idx, threads[idx].core_id,
                threads[idx].frequency, threads[idx].usage.usage);
    }
    fflush(stdout);
}

static char
    *sc_move,
    *sc_init,
    *sc_deinit,
    *sc_cursor_invisible,
    *sc_cursor_normal;

// move cursor into the begin position
void move_cursor(int hpos, int vpos) {
    if (sc_move != NULL) {
        char* go = tgoto(sc_move, hpos, vpos);
        tputs(go, 1, putchar);
    }
}

int main(int argc, char* argv[])
{
    char *termtype = getenv("TERM");
    if (termtype == NULL) {
        termtype = "unknown";
    }
    static char termbuf[2048];
    tgetent(termbuf, termtype);

    // init strings of comands
    sc_init = tgetstr("ti", NULL);
    sc_deinit = tgetstr("te", NULL);
    sc_move = tgetstr("cm", NULL);
    sc_cursor_invisible = tgetstr("vi", NULL);
    sc_cursor_normal = tgetstr("ve", NULL);

    // set custom signal handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_handler = sig_handler;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // run second thread to process key presses
    pthread_t th;
    int ret = pthread_create(&th, NULL, &process_key_press, NULL);
    if (ret != 0) {
        fprintf(stderr, "\npthread_create is failed with: %i\n", ret);
    }

    struct thread_info* threads = NULL;
    int thread_count = 0;

    // init fullscreen mode
    fputs(sc_init, stdout);
    move_cursor(0, 0);

    const char* delim = "--------------------------------------"; 

    // print headers
    puts(delim);
    printf("| %6s | %4s | %10s | %5s |\n", "Thread", "Core", "Speed, MHz", "Usage");
    puts(delim);

    // set invisible cursor
    tputs(sc_cursor_invisible, 1, putchar); 

    pthread_mutex_lock(&mutex1);

    while (true) {
        if (-1 == read_thread_info(&threads, &thread_count)) {
            break;
        }
        
        if (-1 == read_thread_usage(&threads, &thread_count)) {
            break;
        }
 
        print_thread_info(threads, thread_count);
        puts(delim);
 
        // move cursor back
        move_cursor(0, 3);

        // wait for 2 second
        if (wait_for(&cond, &mutex1, 2000000000) != 0) {
            break;
        }
    }

    pthread_mutex_unlock(&mutex1);

    // set cursor normal mode back
    tputs(sc_cursor_normal, 1, putchar); 

    pthread_join(th, NULL);
    // exit fullscreen mode
    fputs(sc_deinit, stdout);

    free(threads);
    return 0;
}

