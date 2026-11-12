-- Reference Lua plugin: uppercases key/text input and commits it.
--
-- The core loads this file as plugin.lua and calls pime.init/process/
-- on_event/shutdown. Host capabilities are exposed on the global `pime`
-- table (see sdk/lua/pime.lua).

local commits = 0

function pime.init(config)
  commits = 0
  pime.log.debug("uppercase", "lua plugin ready")
end

function pime.process(frame)
  local event = frame and frame.event
  local handled = false
  local commands = {}

  if event and (event.type == "key" or event.type == "text")
      and type(event.text) == "string" then
    local text = string.upper(event.text)
    commands[#commands + 1] = { type = "commit", text = text }
    commits = commits + 1
    handled = true
  end

  local out = { handled = handled, state = { commits = commits } }
  if #commands > 0 then
    out.commands = commands
  end
  return out
end

function pime.on_event(event)
  return { handled = false, state = { commits = commits } }
end

function pime.shutdown()
  commits = 0
end
