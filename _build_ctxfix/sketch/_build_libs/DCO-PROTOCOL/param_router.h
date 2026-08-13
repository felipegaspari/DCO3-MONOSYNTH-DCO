#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/_build_libs/DCO-PROTOCOL/param_router.h"
#ifndef PARAM_ROUTER_H
#define PARAM_ROUTER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "params_def.h"

// Generic parameter routing helper.
//
// Each MCU defines a ParamDescriptorT<ValueT> table where:
//   - id    is a ParamId from params_def.h
//   - apply is a function that takes the decoded parameter value
//
// Two dispatch strategies live here; a board uses whichever it wired up.
// The DCO and Mainboard build a 256-entry jump table once (wire ParamId is
// uint8) and dispatch O(1); Input and Screen use the linear scan below.
// Adding a parameter is still: new ParamId + apply_param_* + one table row.

template<typename ValueT>
struct ParamDescriptorT {
  ParamId id;
  void (*apply)(ValueT value);
};

static constexpr uint16_t PARAM_ROUTER_JUMP_SIZE = 256;

template<typename ValueT>
inline void param_router_build_jump(
    void (*(&jump)[PARAM_ROUTER_JUMP_SIZE])(ValueT),
    const ParamDescriptorT<ValueT>* table,
    size_t tableSize)
{
  memset(jump, 0, sizeof(void*) * PARAM_ROUTER_JUMP_SIZE);
  for (size_t i = 0; i < tableSize; ++i) {
    uint16_t id = static_cast<uint16_t>(table[i].id);
    if (id < PARAM_ROUTER_JUMP_SIZE) {
      jump[id] = table[i].apply;
    }
  }
}

template<typename ValueT>
inline void param_router_apply_jump(
    void (*const (&jump)[PARAM_ROUTER_JUMP_SIZE])(ValueT),
    uint16_t rawId,
    ValueT value)
{
  if (rawId < PARAM_ROUTER_JUMP_SIZE && jump[rawId]) {
    jump[rawId](value);
  }
}

// Linear scan (used by Input and Screen, whose tables are short).
template<typename ValueT>
inline void param_router_apply(
    const ParamDescriptorT<ValueT>* table,
    size_t tableSize,
    uint16_t rawId,
    ValueT value)
{
  ParamId id = static_cast<ParamId>(rawId);
  for (size_t i = 0; i < tableSize; ++i) {
    if (table[i].id == id) {
      table[i].apply(value);
      return;
    }
  }
}

// Defined by the boards that build a jump table (DCO, Mainboard); the others
// simply never call it.
void init_param_router();

#endif  // PARAM_ROUTER_H
