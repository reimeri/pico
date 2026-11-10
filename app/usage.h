#ifndef PICO_USAGE_H
#define PICO_USAGE_H

#include "agent_internal.h"
#include "json.h"

bool PicoUsage_Apply(PicoAgent *agent, int input_tokens, int cached_tokens, int *normalized_cached);
bool PicoUsage_SessionPercent(const PicoAgent *agent, int *percent);

/* Shared worker/checkpoint serialization and live/replay presentation. Unknown
 * or malformed measurements are omitted; they never fail compaction. */
void PicoUsage_WriteCompaction(JsonBuf *b, const PicoCompactionUsage *usage);
PicoCompactionUsage PicoUsage_ReadCompaction(const JsonDoc *doc, int obj);
void PicoUsage_FormatCompaction(char *out, size_t cap, const PicoCompactionUsage *usage);

#endif
