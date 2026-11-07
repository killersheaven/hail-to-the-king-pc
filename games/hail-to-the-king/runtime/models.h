#pragma once
#include <stdint.h>
typedef struct { char file[16]; uint32_t type; } model_ent;
typedef struct { float p[3], n[3], uv[2]; } model_vert;
typedef struct { int nv, nt, primary; uint16_t bone0; model_vert *v; uint16_t *idx; } model_part;   /* primary: first part for its bones (alternates follow) */
#define MODEL_MAXPARTS 32
typedef struct {
    int nparts, body;
    model_part parts[MODEL_MAXPARTS];
    float center[3], radius, height;
    uint32_t *tex; int tw, th;      /* RGBA, or NULL */
} model_t;
int models_scan(model_ent *out, int max);   /* characters on the disc */
model_t *model_load(const model_ent *e);
void model_free(model_t *m);
