/*
 * jackoviz.h — public controller API for jackoviz (set_* entry points).
 *
 * Opaque app handle + setters used by the gRPC control plane (grpcstub.cpp).
 */

#ifndef JACKOVIZ_H
#define JACKOVIZ_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Jackoviz Jackoviz;

typedef enum
{
    VIEW_MODE_3D = 0,
    VIEW_MODE_2D = 1,
    VIEW_MODE_1D = 2,
    VIEW_MODE_SCOPE = 3,
} ViewMode;

void set_db_floor(Jackoviz* app, double floor);
void set_db_ceil(Jackoviz* app, double ceil);
void set_view_mode(Jackoviz* app, ViewMode mode);
void set_line_width(Jackoviz* app, float width);
void set_pause(Jackoviz* app, bool paused);
void set_plot_freq(Jackoviz* app, double freq);
void set_kaiser_beta(Jackoviz* app, double beta);

/* Blocking gRPC server (implemented in grpcstub.cpp). Default addr: 0.0.0.0:50051 */
void jvz_grpc_serve(Jackoviz* app, const char* listen_addr);

/* Start jvz_grpc_serve() on a detached background thread. */
void jvz_grpc_serve_async(Jackoviz* app, const char* listen_addr);

#ifdef __cplusplus
}
#endif

#endif /* JACKOVIZ_H */
