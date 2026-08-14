#include "connection_queue.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_failures = 0;

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void expect_eq_int(const char *name, int got, int want) {
    if (got == want) {
        printf("PASS: %s\n", name);
    } else {
        printf("FAIL: %s (got %d want %d)\n", name, got, want);
        g_failures++;
    }
}

static void expect_status(const char *name, connection_queue_status_t got,
                          connection_queue_status_t want) {
    if (got == want) {
        printf("PASS: %s\n", name);
    } else {
        printf("FAIL: %s (got %d want %d)\n", name, (int)got, (int)want);
        g_failures++;
    }
}

/* --- Test 1: FIFO --- */

static void test_fifo_ordering(void) {
    connection_queue_t q;
    expect_eq_int("fifo init", connection_queue_init(&q, 8), 0);

    expect_status("fifo push 10", connection_queue_push(&q, 10, NULL), CONNECTION_QUEUE_OK);
    expect_status("fifo push 20", connection_queue_push(&q, 20, NULL), CONNECTION_QUEUE_OK);
    expect_status("fifo push 30", connection_queue_push(&q, 30, NULL), CONNECTION_QUEUE_OK);

    int fd = -1;
    expect_status("fifo pop 10", connection_queue_pop(&q, &fd), CONNECTION_QUEUE_OK);
    expect_eq_int("fifo value 10", fd, 10);
    expect_status("fifo pop 20", connection_queue_pop(&q, &fd), CONNECTION_QUEUE_OK);
    expect_eq_int("fifo value 20", fd, 20);
    expect_status("fifo pop 30", connection_queue_pop(&q, &fd), CONNECTION_QUEUE_OK);
    expect_eq_int("fifo value 30", fd, 30);

    connection_queue_destroy(&q);
}

/* --- Test 2: wraparound --- */

static void test_circular_wraparound(void) {
    connection_queue_t q;
    expect_eq_int("wrap init", connection_queue_init(&q, 3), 0);

    int fd = -1;
    for (int round = 0; round < 5; round++) {
        int a = 100 + round * 3;
        int b = 101 + round * 3;
        int c = 102 + round * 3;
        expect_status("wrap push a", connection_queue_push(&q, a, NULL), CONNECTION_QUEUE_OK);
        expect_status("wrap push b", connection_queue_push(&q, b, NULL), CONNECTION_QUEUE_OK);
        expect_status("wrap push c", connection_queue_push(&q, c, NULL), CONNECTION_QUEUE_OK);

        expect_status("wrap pop a", connection_queue_pop(&q, &fd), CONNECTION_QUEUE_OK);
        expect_eq_int("wrap val a", fd, a);
        expect_status("wrap pop b", connection_queue_pop(&q, &fd), CONNECTION_QUEUE_OK);
        expect_eq_int("wrap val b", fd, b);
        expect_status("wrap pop c", connection_queue_pop(&q, &fd), CONNECTION_QUEUE_OK);
        expect_eq_int("wrap val c", fd, c);
    }

    connection_queue_destroy(&q);
}

/* --- Test 3: producer blocks when full --- */

typedef struct {
    connection_queue_t *queue;
    int value;
    connection_queue_status_t result;
    pthread_mutex_t *gate_mu;
    pthread_cond_t *gate_cv;
    int *producer_entered;
} push_thread_args_t;

static void *push_thread(void *arg) {
    push_thread_args_t *a = (push_thread_args_t *)arg;

    pthread_mutex_lock(a->gate_mu);
    *a->producer_entered = 1;
    pthread_cond_signal(a->gate_cv);
    pthread_mutex_unlock(a->gate_mu);

    a->result = connection_queue_push(a->queue, a->value, NULL);
    return NULL;
}

static void test_producer_blocks_when_full(void) {
    connection_queue_t q;
    expect_eq_int("full init", connection_queue_init(&q, 1), 0);
    expect_status("full seed", connection_queue_push(&q, 1, NULL), CONNECTION_QUEUE_OK);

    pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
    int producer_entered = 0;

    push_thread_args_t args = {
        .queue = &q,
        .value = 2,
        .result = CONNECTION_QUEUE_ERROR,
        .gate_mu = &gate_mu,
        .gate_cv = &gate_cv,
        .producer_entered = &producer_entered,
    };

    pthread_t tid;
    expect_eq_int("full create", pthread_create(&tid, NULL, push_thread, &args), 0);

    pthread_mutex_lock(&gate_mu);
    while (!producer_entered) {
        pthread_cond_wait(&gate_cv, &gate_mu);
    }
    pthread_mutex_unlock(&gate_mu);

    /* Allow the producer to reach pthread_cond_timedwait on the full queue. */
    sleep_ms(50);

    int fd = -1;
    expect_status("full pop seed", connection_queue_pop(&q, &fd), CONNECTION_QUEUE_OK);
    expect_eq_int("full seed value", fd, 1);

    expect_eq_int("full join", pthread_join(tid, NULL), 0);
    expect_status("full producer ok", args.result, CONNECTION_QUEUE_OK);

    expect_status("full pop second", connection_queue_pop(&q, &fd), CONNECTION_QUEUE_OK);
    expect_eq_int("full second value", fd, 2);

    pthread_cond_destroy(&gate_cv);
    pthread_mutex_destroy(&gate_mu);
    connection_queue_destroy(&q);
}

/* --- Test 4: consumer blocks when empty --- */

typedef struct {
    connection_queue_t *queue;
    int value;
    connection_queue_status_t result;
    pthread_mutex_t *gate_mu;
    pthread_cond_t *gate_cv;
    int *consumer_entered;
} pop_thread_args_t;

static void *pop_thread(void *arg) {
    pop_thread_args_t *a = (pop_thread_args_t *)arg;

    pthread_mutex_lock(a->gate_mu);
    *a->consumer_entered = 1;
    pthread_cond_signal(a->gate_cv);
    pthread_mutex_unlock(a->gate_mu);

    a->result = connection_queue_pop(a->queue, &a->value);
    return NULL;
}

static void test_consumer_blocks_when_empty(void) {
    connection_queue_t q;
    expect_eq_int("empty init", connection_queue_init(&q, 4), 0);

    pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
    int consumer_entered = 0;

    pop_thread_args_t args = {
        .queue = &q,
        .value = -1,
        .result = CONNECTION_QUEUE_ERROR,
        .gate_mu = &gate_mu,
        .gate_cv = &gate_cv,
        .consumer_entered = &consumer_entered,
    };

    pthread_t tid;
    expect_eq_int("empty create", pthread_create(&tid, NULL, pop_thread, &args), 0);

    pthread_mutex_lock(&gate_mu);
    while (!consumer_entered) {
        pthread_cond_wait(&gate_cv, &gate_mu);
    }
    pthread_mutex_unlock(&gate_mu);

    sleep_ms(50);

    expect_status("empty push", connection_queue_push(&q, 42, NULL), CONNECTION_QUEUE_OK);
    expect_eq_int("empty join", pthread_join(tid, NULL), 0);
    expect_status("empty pop ok", args.result, CONNECTION_QUEUE_OK);
    expect_eq_int("empty value", args.value, 42);

    pthread_cond_destroy(&gate_cv);
    pthread_mutex_destroy(&gate_mu);
    connection_queue_destroy(&q);
}

/* --- Test 5: shutdown wakes blocked consumer --- */

static void test_shutdown_wakes_consumer(void) {
    connection_queue_t q;
    expect_eq_int("shc init", connection_queue_init(&q, 4), 0);

    pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
    int consumer_entered = 0;

    pop_thread_args_t args = {
        .queue = &q,
        .value = -1,
        .result = CONNECTION_QUEUE_OK,
        .gate_mu = &gate_mu,
        .gate_cv = &gate_cv,
        .consumer_entered = &consumer_entered,
    };

    pthread_t tid;
    expect_eq_int("shc create", pthread_create(&tid, NULL, pop_thread, &args), 0);

    pthread_mutex_lock(&gate_mu);
    while (!consumer_entered) {
        pthread_cond_wait(&gate_cv, &gate_mu);
    }
    pthread_mutex_unlock(&gate_mu);
    sleep_ms(50);

    connection_queue_shutdown(&q);
    expect_eq_int("shc join", pthread_join(tid, NULL), 0);
    expect_status("shc shutdown status", args.result, CONNECTION_QUEUE_SHUTDOWN);

    pthread_cond_destroy(&gate_cv);
    pthread_mutex_destroy(&gate_mu);
    connection_queue_destroy(&q);
}

/* --- Test 6: shutdown wakes blocked producer --- */

static void test_shutdown_wakes_producer(void) {
    connection_queue_t q;
    expect_eq_int("shp init", connection_queue_init(&q, 1), 0);
    expect_status("shp seed", connection_queue_push(&q, 1, NULL), CONNECTION_QUEUE_OK);

    pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
    int producer_entered = 0;

    push_thread_args_t args = {
        .queue = &q,
        .value = 99,
        .result = CONNECTION_QUEUE_OK,
        .gate_mu = &gate_mu,
        .gate_cv = &gate_cv,
        .producer_entered = &producer_entered,
    };

    pthread_t tid;
    expect_eq_int("shp create", pthread_create(&tid, NULL, push_thread, &args), 0);

    pthread_mutex_lock(&gate_mu);
    while (!producer_entered) {
        pthread_cond_wait(&gate_cv, &gate_mu);
    }
    pthread_mutex_unlock(&gate_mu);
    sleep_ms(50);

    connection_queue_shutdown(&q);
    expect_eq_int("shp join", pthread_join(tid, NULL), 0);
    expect_status("shp shutdown status", args.result, CONNECTION_QUEUE_SHUTDOWN);

    int fd = -1;
    expect_status("shp drain seed", connection_queue_pop(&q, &fd), CONNECTION_QUEUE_OK);
    expect_eq_int("shp seed value", fd, 1);
    expect_status("shp empty after drain", connection_queue_pop(&q, &fd),
                  CONNECTION_QUEUE_SHUTDOWN);

    pthread_cond_destroy(&gate_cv);
    pthread_mutex_destroy(&gate_mu);
    connection_queue_destroy(&q);
}

/* --- Test 7: drain after shutdown --- */

static void test_drain_after_shutdown(void) {
    connection_queue_t q;
    expect_eq_int("drain init", connection_queue_init(&q, 8), 0);

    expect_status("drain push 7", connection_queue_push(&q, 7, NULL), CONNECTION_QUEUE_OK);
    expect_status("drain push 8", connection_queue_push(&q, 8, NULL), CONNECTION_QUEUE_OK);
    expect_status("drain push 9", connection_queue_push(&q, 9, NULL), CONNECTION_QUEUE_OK);

    connection_queue_shutdown(&q);

    int fd = -1;
    expect_status("drain pop 7", connection_queue_pop(&q, &fd), CONNECTION_QUEUE_OK);
    expect_eq_int("drain val 7", fd, 7);
    expect_status("drain pop 8", connection_queue_pop(&q, &fd), CONNECTION_QUEUE_OK);
    expect_eq_int("drain val 8", fd, 8);
    expect_status("drain pop 9", connection_queue_pop(&q, &fd), CONNECTION_QUEUE_OK);
    expect_eq_int("drain val 9", fd, 9);
    expect_status("drain end", connection_queue_pop(&q, &fd), CONNECTION_QUEUE_SHUTDOWN);

    connection_queue_destroy(&q);
}

/* --- Abort flag unblocks full push (signal-safe shutdown path) --- */

typedef struct {
    connection_queue_t *queue;
    const volatile sig_atomic_t *flag;
    connection_queue_status_t result;
} abort_push_args_t;

static void *abort_push_thread(void *arg) {
    abort_push_args_t *a = (abort_push_args_t *)arg;
    a->result = connection_queue_push(a->queue, 2, a->flag);
    return NULL;
}

static void test_abort_flag_unblocks_push(void) {
    connection_queue_t q;
    expect_eq_int("abort init", connection_queue_init(&q, 1), 0);
    expect_status("abort seed", connection_queue_push(&q, 1, NULL), CONNECTION_QUEUE_OK);

    volatile sig_atomic_t abort_flag = 0;
    abort_push_args_t args = {.queue = &q, .flag = &abort_flag, .result = CONNECTION_QUEUE_OK};

    pthread_t tid;
    expect_eq_int("abort create", pthread_create(&tid, NULL, abort_push_thread, &args), 0);
    sleep_ms(150);
    abort_flag = 1;
    expect_eq_int("abort join", pthread_join(tid, NULL), 0);
    expect_status("abort status", args.result, CONNECTION_QUEUE_SHUTDOWN);

    int fd = -1;
    expect_status("abort drain", connection_queue_pop(&q, &fd), CONNECTION_QUEUE_OK);
    expect_eq_int("abort seed remains", fd, 1);

    connection_queue_destroy(&q);
}

int main(void) {
    test_fifo_ordering();
    test_circular_wraparound();
    test_producer_blocks_when_full();
    test_consumer_blocks_when_empty();
    test_shutdown_wakes_consumer();
    test_shutdown_wakes_producer();
    test_drain_after_shutdown();
    test_abort_flag_unblocks_push();

    if (g_failures != 0) {
        printf("%d connection_queue test(s) failed\n", g_failures);
        return 1;
    }
    printf("All connection_queue tests passed.\n");
    return 0;
}
