// d9mt: Metal backend — completion watcher (liveness backbone).
//
// One background thread per process waits on submitted MTLCommandBuffers in
// FIFO order and runs registered callbacks when they retire. This implements
// the "completion handler" role of BACKEND-SURFACE §5.1 on top of winemetal,
// which only exposes MTLCommandBuffer_waitUntilCompleted/_status (no generic
// callback export). All three deadlock-critical signals (presenter frame,
// submission fence, staging fence) and resource track-release run through
// here.

#include <queue>
#include <utility>

#include "d9mt_backend.h"

#include "../../vendor/dxvk/src/util/thread.h"
#include "../../vendor/dxvk/src/util/util_env.h"
#include "../../vendor/dxvk/src/util/log/log.h"

namespace dxvk::d9mt {

  namespace {

    struct WatchEntry {
      obj_handle_t          cmdbuf = 0;
      std::function<void()> callback;
    };

    class CompletionWatcher {

    public:

      CompletionWatcher()
      : m_thread([this] { run(); }) {
      }

      // Never destroyed (allocated with new, intentionally leaked): joining
      // a thread from a static destructor during PE process teardown under
      // Wine is a known hang source. waitIdle() is the orderly drain.
      ~CompletionWatcher() = delete;

      void watch(obj_handle_t cmdbuf, std::function<void()>&& callback) {
        if (cmdbuf)
          NSObject_retain(cmdbuf);

        std::unique_lock<dxvk::mutex> lock(m_mutex);
        m_queue.push(WatchEntry { cmdbuf, std::move(callback) });
        m_workCond.notify_one();
      }

      void waitIdle() {
        std::unique_lock<dxvk::mutex> lock(m_mutex);
        m_idleCond.wait(lock, [this] {
          return m_queue.empty() && !m_busy;
        });
      }

    private:

      dxvk::mutex              m_mutex;
      dxvk::condition_variable m_workCond;
      dxvk::condition_variable m_idleCond;

      std::queue<WatchEntry>   m_queue;
      bool                     m_busy = false;

      dxvk::thread             m_thread;

      void run() {
        env::setThreadName("d9mt-watcher");

        std::unique_lock<dxvk::mutex> lock(m_mutex);

        while (true) {
          m_workCond.wait(lock, [this] {
            return !m_queue.empty();
          });

          WatchEntry entry = std::move(m_queue.front());
          m_queue.pop();
          m_busy = true;

          lock.unlock();

          if (entry.cmdbuf) {
            MTLCommandBuffer_waitUntilCompleted(entry.cmdbuf);

            if (MTLCommandBuffer_status(entry.cmdbuf) == WMTCommandBufferStatusError) {
              Logger::err("d9mt: command buffer completed with error");
              logNSError("d9mt: MTLCommandBuffer error",
                MTLCommandBuffer_error(entry.cmdbuf));
            }
          }

          if (entry.callback)
            entry.callback();

          if (entry.cmdbuf)
            NSObject_release(entry.cmdbuf);

          lock.lock();
          m_busy = false;

          if (m_queue.empty())
            m_idleCond.notify_all();
        }
      }

    };

    CompletionWatcher& watcher() {
      // intentionally leaked, see ~CompletionWatcher comment
      static CompletionWatcher* s_watcher = new CompletionWatcher();
      return *s_watcher;
    }

  } // anonymous namespace


  void watchCommandBuffer(obj_handle_t cmdbuf, std::function<void()> callback) {
    watcher().watch(cmdbuf, std::move(callback));
  }


  void watcherWaitIdle() {
    watcher().waitIdle();
  }

}
