/* The vertex variant to draw a CoreAnimation pipeline with when the selected one leaves a varying the fragment reads
 * unserved (spec metal-ca-render-glitch, design Fix 1). Plain C, no Objective-C, so the host tests can include it.
 *
 * Order: SELECTED if it covers (KEPT); else the pipeline table's vertex (PIPELINE), the library's pairing for the
 * fragment (PAIRED), then LIBRARY_VERTICES in the given order (SUPERSET); SELECTED with NONE if nothing covers.
 *
 * NULL entries and labels already asked are skipped, so COVERS is called at most once per distinct label; an answer
 * other than 1 (including -1) counts as not covering. With no COVERS, SELECTED is returned with KEPT (no guard). */
#ifndef HM_CA_COVER_H
#define HM_CA_COVER_H

#include <stddef.h>
#include <string.h>

enum hm_ca_cover_how { HM_CA_COVER_KEPT, HM_CA_COVER_PIPELINE, HM_CA_COVER_PAIRED, HM_CA_COVER_SUPERSET, HM_CA_COVER_NONE };

/* 1: VERTEX writes every identity the fragment reads (and is usable); 0: it does not; -1: cannot tell (treated as 0) */
typedef int (*hm_ca_covers_fn)(void *ctx, const char *vertex);

/* 1 if LABEL equals one of the first N entries of TRIED */
static inline int
hm_ca_cover_tried(const char *label, const char *const *tried, size_t n)
{
for (size_t i = 0; i < n; i++) {
if (tried[i] != NULL && strcmp(tried[i], label) == 0) {
return 1;
}
}
return 0;
}

static inline const char *
hm_ca_cover_vertex(const char *selected, const char *pipeline_vertex, const char *paired_vertex,
   const char *const *library_vertices, size_t count, hm_ca_covers_fn covers, void *ctx,
   enum hm_ca_cover_how *how)
{
if (how != NULL) {
*how = HM_CA_COVER_NONE;
}
if (covers == NULL) {
if (how != NULL) {
*how = HM_CA_COVER_KEPT;
}
return selected;
}
if (selected != NULL && covers(ctx, selected) == 1) {
if (how != NULL) {
*how = HM_CA_COVER_KEPT;
}
return selected;
}
/* Candidates already asked: SELECTED, PIPELINE_VERTEX, PAIRED_VERTEX; library entries are compared in place */
const char *fixed[3] = { selected, NULL, NULL };
size_t nfixed = 1;
const char *cand[2] = { pipeline_vertex, paired_vertex };
const enum hm_ca_cover_how cand_how[2] = { HM_CA_COVER_PIPELINE, HM_CA_COVER_PAIRED };
for (size_t c = 0; c < 2; c++) {
const char *v = cand[c];
if (v == NULL || hm_ca_cover_tried(v, fixed, nfixed)) {
continue;
}
fixed[nfixed++] = v;
if (covers(ctx, v) == 1) {
if (how != NULL) {
*how = cand_how[c];
}
return v;
}
}
for (size_t i = 0; library_vertices != NULL && i < count; i++) {
const char *v = library_vertices[i];
if (v == NULL || hm_ca_cover_tried(v, fixed, nfixed) || hm_ca_cover_tried(v, library_vertices, i)) {
continue;
}
if (covers(ctx, v) == 1) {
if (how != NULL) {
*how = HM_CA_COVER_SUPERSET;
}
return v;
}
}
return selected;
}
#endif /* HM_CA_COVER_H */
