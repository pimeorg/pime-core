--- pime Lua plugin SDK (Lua 5.4).
--- Host functions are injected by the core; this file documents and type-stubs
--- them for editors and for the conformance linter. Byte data crosses the
--- boundary as Lua strings; CBOR tables map one-to-one to Lua tables.
---
--- Lifecycle the core calls:
---   pime.init(config) -> nil | nil, err
---   pime.process(input) -> output
---   pime.on_event(event) -> output
---   pime.shutdown()

local pime = {}

pime.VERSION = "1.0"

--- Logging ----------------------------------------------------------------
--- pime.log(level, tag, msg) with level in {"debug","info","warn","error"}.
function pime.log(level, tag, msg) end
pime.log.debug = function(tag, msg) end
pime.log.info = function(tag, msg) end
pime.log.warn = function(tag, msg) end
pime.log.error = function(tag, msg) end

--- Workspace (shared inside one generation) --------------------------------
pime.workspace = {
    set = function(key, value) end,
    get = function(key) end,
}

--- Store (per-plugin namespace) -------------------------------------------
pime.store = {
    set = function(key, value) end,
    get = function(key) end,
    delete = function(key) end,
    batch = function(ops) end,
    scan = function(prefix, limit) end, -- iterator
    quota = function() end,             -- used, limit
    flush = function() end,
}

--- Assets -----------------------------------------------------------------
pime.asset = {
    open = function(name) end,
    size = function(handle) end,
    read = function(handle, offset, len) end,
    map = function(handle, offset, len) end,
    close = function(handle) end,
}

--- Config -----------------------------------------------------------------
pime.config = {
    get = function(key) end,
    list = function(prefix) end,
    snapshot = function() end,
}

--- Time / random / timer --------------------------------------------------
pime.time = {
    monotonic_ms = function() end,
    wall_ms = function() end,
    timezone_offset = function() end,
}
pime.random = {
    bytes = function(n) end,
}
pime.timer = {
    after = function(delay_ms, id) end,
    every = function(period_ms, id) end,
    cancel = function(handle) end,
}

--- Capabilities -----------------------------------------------------------
pime.caps = {
    has = function(capability) end,
    list = function() end,
}

--- Threads ----------------------------------------------------------------
pime.thread = {
    create = function(desc) end, -- { name, entry, priority, stack_kb }
    post = function(tid, message) end,
    recv = function(tid, timeout_ms) end,
    park = function(tid, timeout_ms) end,
    unpark = function(tid) end,
    close = function(tid) end,
    join = function(tid, timeout_ms) end,
}
pime.emit = function(session_id, output) end

--- Contexts, channels, sync, shared memory --------------------------------
pime.ctx = {
    acquire = function(name, kind, generation, options) end,
    release = function(handle) end,
    pin = function(handle, pinned) end,
    drop = function(name, kind) end,
    info = function(name, kind) end,
    call = function(handle, method, input) end,
}
pime.channel = {
    create = function(capacity) end,
    send = function(handle, message, timeout_ms) end,
    recv = function(handle, timeout_ms) end,
    close = function(handle, mode) end,
}
pime.sync = {
    mutex = function(options) end,
    condvar = function() end,
}
pime.shm = {
    create = function(bytes) end,
    open = function(handle) end,
}

--- Audio ------------------------------------------------------------------
pime.audio = {
    capture = function(format) end,
    play = function(format) end,
    pcm = { convert = function(frame, from, to) end },
    opus = {
        encode = function(pcm, options) end,
        decode = function(packet, options) end,
    },
}

--- ML ---------------------------------------------------------------------
pime.ml = {
    load = function(model_id, options) end,
    run = function(session, tensor) end,
    run_async = function(session, tensor) end,
    cancel = function(job) end,
    free = function(session) end,
}

--- Crypto -----------------------------------------------------------------
pime.crypto = {
    digest = function(alg, data) end,
    mac = function(alg, key, data) end,
    cipher = function(alg, key, iv, aad, plaintext) end,
    sign = function(key, alg, data) end,
    verify = function(public_key, alg, data, signature) end,
    kdf = function(alg, key, salt, info, length) end,
    x509_verify = function(cert, ca) end,
    random = function(n) end,
    key_import = function(format, data) end,
    key_destroy = function(handle) end,
}

--- Net --------------------------------------------------------------------
pime.net = {
    request = function(request) end,
    result = function(id) end,
    open = function(protocol, url, options) end,
    send = function(handle, data) end,
    close = function(handle) end,
}

return pime
