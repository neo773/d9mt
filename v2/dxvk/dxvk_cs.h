#pragma once

// Command-stream shim: DxvkCsChunk + DxvkCsThread, SYNCHRONOUS (no worker thread).
//
// The real DXVK records D3D9 commands into fixed-size chunks on the app thread,
// then hands those chunks to a background CS thread that replays each recorded
// lambda against a single DxvkContext. This shim keeps the *exact* chunk API the
// frontend's EmitCs/EmitCsCmd templates rely on (push / pushCmd / pushData /
// executeAll / init / reset, and DxvkCsChunkRef pooling), but collapses the
// thread: dispatchChunk replays the chunk immediately against the context the
// DxvkCsThread was constructed with. Synchronous replay means draws reach Metal
// in submission order with no cross-thread synchronization. See SHIM_SPEC.md.

#include <cstdint>
#include <new>
#include <vector>

#include "dxvk_include.h"   // util layer (Rc, Flags, mutex, align, likely/unlikely)

namespace dxvk {

  // Forward declaration — the recorded lambdas all take a DxvkContext*. The
  // context is the actual shim that turns recorded calls into Metal commands.
  class DxvkContext;
  class DxvkDevice;

  // Chunk capacity. Commands (a small vtable'd functor each, plus any inline
  // data block) are bump-allocated into this fixed buffer; when it fills, the
  // frontend rolls over to a fresh chunk. Matches the reference value so the
  // frontend's batching heuristics behave identically.
  constexpr static size_t DxvkCsChunkSize = 16384;


  /**
   * \brief Command stream operation
   *
   * Type-erased base for one recorded command. Commands are singly linked so a
   * chunk can replay them in record order without a separate index.
   */
  class DxvkCsCmd {

  public:

    virtual ~DxvkCsCmd() { }

    DxvkCsCmd* next() const {
      return m_next;
    }

    DxvkCsCmd** chain() {
      return &m_next;
    }

    // Replays this command against the shim context.
    virtual void exec(DxvkContext* ctx) = 0;

  private:

    DxvkCsCmd* m_next = nullptr;

  };


  /**
   * \brief Typed command
   *
   * Wraps a callable `void(DxvkContext*)` recorded via DxvkCsChunk::push. The
   * functor is move-constructed in place inside the chunk's data buffer, so no
   * heap allocation happens per command.
   */
  template<typename T>
  class DxvkCsTypedCmd : public DxvkCsCmd {

  public:

    DxvkCsTypedCmd(T&& cmd)
    : m_command(std::move(cmd)) { }

    DxvkCsTypedCmd             (DxvkCsTypedCmd&&) = delete;
    DxvkCsTypedCmd& operator = (DxvkCsTypedCmd&&) = delete;

    void exec(DxvkContext* ctx) {
      m_command(ctx);
    }

  private:

    T m_command;

  };


  /**
   * \brief Command data block
   *
   * Backs a growing array of POD structures stored inline after a command (used
   * by EmitCsCmd, e.g. draw argument batching). The block locates its own data
   * by self-relative offset so no pointer needs storing.
   */
  class DxvkCsDataBlock {
    friend class DxvkCsChunk;
  public:

    size_t count() const {
      return m_structCount;
    }

    void* first() {
      return reinterpret_cast<char*>(this) + m_dataOffset;
    }

    void* at(uint32_t idx) {
      return reinterpret_cast<char*>(this) + m_dataOffset + idx * uint32_t(m_structSize);
    }

  private:

    uint32_t m_dataOffset  = 0u;
    uint16_t m_structSize  = 0u;
    uint16_t m_structCount = 0u;

  };


  /**
   * \brief Typed command with metadata
   *
   * Wraps a callable `void(DxvkContext*, M* data, size_t count)` plus an inline
   * array of M structures that the frontend may keep appending to after the
   * command was recorded (draw batching). Destroys the M array on chunk reset.
   */
  template<typename T, typename M>
  class DxvkCsDataCmd : public DxvkCsCmd {

  public:

    DxvkCsDataCmd(T&& cmd)
    : m_command(std::move(cmd)) { }

    ~DxvkCsDataCmd() {
      auto data = reinterpret_cast<M*>(m_data.first());

      for (size_t i = 0; i < m_data.count(); i++)
        data[i].~M();
    }

    DxvkCsDataCmd             (DxvkCsDataCmd&&) = delete;
    DxvkCsDataCmd& operator = (DxvkCsDataCmd&&) = delete;

    void exec(DxvkContext* ctx) {
      // Non-const M* so the recorded functor may move out of the data array.
      m_command(ctx, reinterpret_cast<M*>(m_data.first()), m_data.count());
    }

    DxvkCsDataBlock* data() {
      return &m_data;
    }

  private:

    alignas(std::max(alignof(T), alignof(M)))
    T               m_command;
    DxvkCsDataBlock m_data;

  };


  /**
   * \brief Submission flags
   */
  enum class DxvkCsChunkFlag : uint32_t {
    // Chunk is only submitted once and can be recycled right after replay.
    SingleUse,
  };

  using DxvkCsChunkFlags = Flags<DxvkCsChunkFlag>;


  /**
   * \brief Command chunk
   *
   * A fixed-size bump allocator holding a linked list of recorded commands.
   * push() consumes a callable; pushCmd()/pushData() back the EmitCsCmd path.
   * The bodies are reproduced verbatim from reference DXVK — they are pure CPU
   * memory management and contain no Vulkan, so the shim keeps them as-is.
   */
  class DxvkCsChunk : public RcObject {

  public:

    DxvkCsChunk() { }
    ~DxvkCsChunk() { this->reset(); }

    bool empty() const {
      return m_commandOffset == 0;
    }

    // Records a callable. Returns false when the chunk is full, signalling the
    // frontend to roll over to a fresh chunk and retry.
    template<typename T>
    bool push(T& command) {
      using FuncType = DxvkCsTypedCmd<T>;
      void* ptr = alloc<FuncType>(0u);

      if (unlikely(!ptr))
        return false;

      auto next = new (ptr) FuncType(std::move(command));
      append(next);
      return true;
    }

    template<typename T>
    bool push(T&& command) {
      return push(command);
    }

    // Records a callable plus an inline array of `count` M structures. Returns
    // the data block (or nullptr if the chunk is full).
    template<typename M, typename T>
    DxvkCsDataBlock* pushCmd(T& command, size_t count) {
      size_t dataSize = count * sizeof(M);

      using FuncType = DxvkCsDataCmd<T, M>;
      void* ptr = alloc<FuncType>(dataSize);

      if (unlikely(!ptr))
        return nullptr;

      auto next = new (ptr) FuncType(std::move(command));
      append(next);

      // Self-relative offset from the block to its data, which was bump
      // allocated immediately after the command object.
      auto block = next->data();
      block->m_dataOffset = reinterpret_cast<uintptr_t>(&m_data[m_commandOffset - dataSize])
                          - reinterpret_cast<uintptr_t>(block);
      block->m_structSize = sizeof(M);
      block->m_structCount = count;
      return block;
    }

    // Grows the most-recently-pushed data block by `count` structures. Used by
    // the frontend to batch additional draws into an existing draw command.
    void* pushData(DxvkCsDataBlock* block, uint32_t count) {
      uint32_t dataSize = block->m_structSize * count;

      if (unlikely(m_commandOffset + dataSize > DxvkCsChunkSize))
        return nullptr;

      void* ptr = &m_data[m_commandOffset];
      m_commandOffset += dataSize;

      block->m_structCount += count;
      return ptr;
    }

    void init(DxvkCsChunkFlags flags) {
      m_flags = flags;
    }

    // Replays every recorded command against the context, then resets so the
    // chunk can be reused. Synchronous: this is the whole "CS execution".
    void executeAll(DxvkContext* ctx) {
      for (auto cmd = m_head; cmd != nullptr; cmd = cmd->next())
        cmd->exec(ctx);

      this->reset();
    }

    // Destroys all recorded commands and rewinds the bump allocator.
    void reset() {
      for (auto cmd = m_head; cmd != nullptr; ) {
        auto next = cmd->next();
        cmd->~DxvkCsCmd();
        cmd = next;
      }

      m_head = nullptr;
      m_next = &m_head;
      m_commandOffset = 0;
    }

  private:

    size_t m_commandOffset = 0;

    DxvkCsCmd*  m_head = nullptr;
    DxvkCsCmd** m_next = &m_head;

    DxvkCsChunkFlags m_flags;

    alignas(64)
    char m_data[DxvkCsChunkSize];

    template<typename T>
    void* alloc(size_t extra) {
      if (alignof(T) > alignof(DxvkCsCmd))
        m_commandOffset = dxvk::align(m_commandOffset, alignof(T));

      if (unlikely(m_commandOffset + sizeof(T) + extra > DxvkCsChunkSize))
        return nullptr;

      void* result = &m_data[m_commandOffset];
      m_commandOffset += sizeof(T) + extra;
      return result;
    }

    void append(DxvkCsCmd* cmd) {
      *m_next = cmd;
      m_next = cmd->chain();
    }

  };


  /**
   * \brief Chunk pool
   *
   * Recycles chunks to avoid per-frame allocation. Identical semantics to the
   * reference pool.
   */
  class DxvkCsChunkPool {

  public:

    DxvkCsChunkPool() { }

    ~DxvkCsChunkPool() {
      for (auto chunk : m_chunks)
        delete chunk;
    }

    DxvkCsChunkPool             (const DxvkCsChunkPool&) = delete;
    DxvkCsChunkPool& operator = (const DxvkCsChunkPool&) = delete;

    DxvkCsChunk* allocChunk(DxvkCsChunkFlags flags) {
      DxvkCsChunk* chunk = nullptr;

      { std::lock_guard<dxvk::mutex> lock(m_mutex);

        if (!m_chunks.empty()) {
          chunk = m_chunks.back();
          m_chunks.pop_back();
        }
      }

      if (!chunk)
        chunk = new DxvkCsChunk();

      chunk->init(flags);
      return chunk;
    }

    void freeChunk(DxvkCsChunk* chunk) {
      chunk->reset();

      std::lock_guard<dxvk::mutex> lock(m_mutex);
      m_chunks.push_back(chunk);
    }

  private:

    dxvk::mutex               m_mutex;
    std::vector<DxvkCsChunk*> m_chunks;

  };


  /**
   * \brief Chunk reference
   *
   * Ref-counts a chunk and returns it to its pool when the last reference is
   * dropped. The frontend moves these into EmitCsChunk; the shim thread replays
   * and releases them. Reproduced verbatim — pure ref-counting, no Vulkan.
   */
  class DxvkCsChunkRef {

  public:

    DxvkCsChunkRef() { }
    DxvkCsChunkRef(
      DxvkCsChunk*      chunk,
      DxvkCsChunkPool*  pool)
    : m_chunk (chunk),
      m_pool  (pool) {
      this->incRef();
    }

    DxvkCsChunkRef(const DxvkCsChunkRef& other)
    : m_chunk (other.m_chunk),
      m_pool  (other.m_pool) {
      this->incRef();
    }

    DxvkCsChunkRef(DxvkCsChunkRef&& other)
    : m_chunk (other.m_chunk),
      m_pool  (other.m_pool) {
      other.m_chunk = nullptr;
      other.m_pool  = nullptr;
    }

    DxvkCsChunkRef& operator = (const DxvkCsChunkRef& other) {
      other.incRef();
      this->decRef();
      this->m_chunk = other.m_chunk;
      this->m_pool  = other.m_pool;
      return *this;
    }

    DxvkCsChunkRef& operator = (DxvkCsChunkRef&& other) {
      this->decRef();
      this->m_chunk = other.m_chunk;
      this->m_pool  = other.m_pool;
      other.m_chunk = nullptr;
      other.m_pool  = nullptr;
      return *this;
    }

    ~DxvkCsChunkRef() {
      this->decRef();
    }

    DxvkCsChunk* operator -> () const {
      return m_chunk;
    }

    explicit operator bool () const {
      return m_chunk != nullptr;
    }

  private:

    DxvkCsChunk*      m_chunk = nullptr;
    DxvkCsChunkPool*  m_pool  = nullptr;

    void incRef() const {
      if (m_chunk != nullptr)
        m_chunk->incRef();
    }

    void decRef() const {
      if (m_chunk != nullptr && m_chunk->decRef() == 0)
        m_pool->freeChunk(m_chunk);
    }

  };


  /**
   * \brief Queue type
   *
   * Kept for API parity with the frontend/reference. The shim executes
   * everything inline regardless of queue, in submission order.
   */
  enum class DxvkCsQueue : uint32_t {
    Ordered       = 0,
    HighPriority  = 1,
  };


  /**
   * \brief Command stream thread (synchronous shim)
   *
   * In real DXVK this spawns a worker that replays chunks against a context.
   * The shim runs everything on the calling thread: dispatchChunk replays the
   * chunk immediately against the context handed to the constructor, so the
   * frontend's "present from the CS thread" pattern just executes inline.
   *
   * The frontend constructs this as `m_csThread(dxvkDevice, dxvkDevice->createContext())`.
   */
  class DxvkCsThread {

  public:

    constexpr static uint64_t SynchronizeAll = ~0ull;

    DxvkCsThread(
      const Rc<DxvkDevice>&   device,
      const Rc<DxvkContext>&  context)
    : m_device (device),
      m_context(context) { }

    ~DxvkCsThread() { }

    // Replays the chunk now against the stored context and returns the bumped
    // sequence number. No real dispatch — execution is complete on return.
    uint64_t dispatchChunk(DxvkCsChunkRef&& chunk) {
      if (chunk)
        chunk->executeAll(m_context.ptr());

      return ++m_seq;
    }

    // Out-of-band injection. Same synchronous replay; `synchronize` is moot
    // because execution already finished by the time we return.
    void injectChunk(
            DxvkCsQueue       /*queue*/,
            DxvkCsChunkRef&&  chunk,
            bool              /*synchronize*/) {
      if (chunk)
        chunk->executeAll(m_context.ptr());

      ++m_seq;
    }

    // Already synchronous — nothing to wait for.
    void synchronize(uint64_t /*seq*/) { }

    uint64_t lastSequenceNumber() const {
      return m_seq;
    }

  private:

    Rc<DxvkDevice>  m_device;
    Rc<DxvkContext> m_context;

    // Monotonic sequence counter so lastSequenceNumber()/dispatch return values
    // stay meaningful to the frontend even though replay is inline.
    uint64_t        m_seq = 0u;

  };

}
