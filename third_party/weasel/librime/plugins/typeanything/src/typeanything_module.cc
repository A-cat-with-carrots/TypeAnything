// TypeAnything Rime plugin — module registration.
//
// Registers our custom processor that intercepts Enter to translate the
// committed Chinese composition into a target language via DeepSeek API.

#include <rime_api.h>
#include <rime/common.h>
#include <rime/registry.h>

#include "typeanything_processor.h"

using namespace rime;

static void rime_typeanything_initialize() {
  LOG(INFO) << "registering components from module 'typeanything'.";
  Registry& r = Registry::instance();
  r.Register("typeanything_processor", new Component<typeanything::TypeAnythingProcessor>);
}

static void rime_typeanything_finalize() {}

RIME_REGISTER_MODULE(typeanything)
