#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

typedef struct {
    size_t len;
    double* x;
    double* y;
    double* vx;
    double* vy;
} Particle_SoA;

typedef struct {
    int64_t start;
    int64_t end;
    Particle_SoA* pts;
    double dt;
} WorkerArg;

static void* worker(void* arg) {
    WorkerArg* w = (WorkerArg*)arg;
    double* __restrict__ x = w->pts->x;
    double* __restrict__ y = w->pts->y;
    const double* __restrict__ vx = w->pts->vx;
    const double* __restrict__ vy = w->pts->vy;
    double dt = w->dt;

    for (int64_t i = w->start; i < w->end; i++) {
        x[i] += vx[i] * dt;
        y[i] += vy[i] * dt;
    }
    return NULL;
}

int main(void) {
    size_t n = 100000;
    size_t steps = 10;
    double dt = 0.01;

    Particle_SoA pts;
    pts.len = n;
    pts.x = (double*)malloc(n * sizeof(double));
    pts.y = (double*)malloc(n * sizeof(double));
    pts.vx = (double*)malloc(n * sizeof(double));
    pts.vy = (double*)malloc(n * sizeof(double));

    for (size_t i = 0; i < n; i++) {
        pts.x[i] = i * 0.001;
        pts.y[i] = i * 0.002;
        pts.vx[i] = 1.5;
        pts.vy[i] = -2.5;
    }

    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs <= 0) nprocs = 4;
    pthread_t* threads = (pthread_t*)malloc(nprocs * sizeof(pthread_t));
    WorkerArg* args = (WorkerArg*)malloc(nprocs * sizeof(WorkerArg));

    for (size_t s = 0; s < steps; s++) {
        int64_t chunk = (int64_t)n / nprocs;
        for (long t = 0; t < nprocs; t++) {
            args[t].start = t * chunk;
            args[t].end = (t == nprocs - 1) ? (int64_t)n : (t + 1) * chunk;
            args[t].pts = &pts;
            args[t].dt = dt;
            pthread_create(&threads[t], NULL, worker, &args[t]);
        }
        for (long t = 0; t < nprocs; t++) {
            pthread_join(threads[t], NULL);
        }
    }

    printf("state [pts.x[0]] = %g\n", pts.x[0]);
    printf("state [pts.y[0]] = %g\n", pts.y[0]);
    printf("state [pts.x[99999]] = %g\n", pts.x[99999]);
    printf("state [pts.y[99999]] = %g\n", pts.y[99999]);

    free(pts.x);
    free(pts.y);
    free(pts.vx);
    free(pts.vy);
    free(threads);
    free(args);
    return 0;
}
