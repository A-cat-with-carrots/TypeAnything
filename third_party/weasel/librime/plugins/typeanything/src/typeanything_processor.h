// TypeAnything processor — captures committed Chinese, on Enter calls LLM,
// replaces accumulated Chinese in the app textbox with translation.

#ifndef TYPEANYTHING_PROCESSOR_H_
#define TYPEANYTHING_PROCESSOR_H_

#include <rime/processor.h>
#include <rime/component.h>
#include <rime/common.h>
#include <boost/signals2/connection.hpp>
#include <atomic>
#include <string>

namespace rime {
class Context;
}

namespace typeanything {

class TypeAnythingProcessor : public rime::Processor {
 public:
  explicit TypeAnythingProcessor(const rime::Ticket& ticket);
  ~TypeAnythingProcessor() override;

  rime::ProcessResult ProcessKeyEvent(const rime::KeyEvent& key_event) override;

 private:
  void LoadConfig();
  std::string ResolveTargetLang() const;
  void OnCommit(rime::Context* ctx);
  // Async dispatch — spawns worker thread, returns immediately after
  // putting placeholder in app.
  void DispatchTranslate(const std::string& chinese);

  std::string api_key_;
  std::string model_;
  std::string endpoint_;
  std::string endpoint_path_;
  std::string default_target_lang_;
  double temperature_ = 0.3;

  std::string accumulated_;
  std::atomic<bool> suppress_capture_{false};
  // Versioning: each translation request gets an id; when the worker
  // completes we only apply if request_id_ == id at end.
  std::atomic<uint64_t> request_id_{0};

  boost::signals2::connection commit_conn_;
};

}  // namespace typeanything

#endif  // TYPEANYTHING_PROCESSOR_H_
