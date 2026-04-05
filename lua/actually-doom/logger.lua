local fs = vim.fs
local uv = vim.uv

local M = {}

local xdg_cache_home = vim.env.XDG_CACHE_HOME
  or fs.joinpath(uv.os_homedir(), ".cache")
local log_dir = fs.joinpath(xdg_cache_home, "actually-doom.nvim")

local state = {
  ready = false,
  path = nil,
  warned = false,
}

local function notify_once(msg)
  if state.warned then
    return
  end
  state.warned = true
  vim.schedule(function()
    vim.notify(("[actually-doom.nvim] %s"):format(msg), vim.log.levels.WARN)
  end)
end

local function write_raw(data)
  local file, err = io.open(state.path, "a")
  if not file then
    notify_once(("failed to open log file for writing: %s"):format(err))
    return
  end

  local ok, write_err = file:write(data)
  file:flush()
  file:close()

  if not ok then
    notify_once(("failed to write log data: %s"):format(write_err))
  end
end

local function ensure_ready()
  if state.ready then
    return true
  end

  local ok, err = pcall(vim.fn.mkdir, log_dir, "p")
  if not ok then
    notify_once(("failed to create log directory %q: %s"):format(log_dir, err))
    return false
  end

  local session_id = ("%s-%d-%d"):format(
    os.date "%Y%m%d-%H%M%S",
    uv.os_getpid(),
    math.floor((uv.hrtime() / 1000000) % 1000)
  )
  state.path = fs.joinpath(log_dir, ("session-%s.log"):format(session_id))
  state.ready = true

  write_raw(
    ("[%s] [logger] session started (nvim pid=%d)\n"):format(
      os.date "%Y-%m-%d %H:%M:%S",
      uv.os_getpid()
    )
  )
  return true
end

function M.path()
  if not ensure_ready() then
    return nil
  end
  return state.path
end

--- @param source string
--- @param text string
function M.log(source, text)
  if text == "" or not ensure_ready() then
    return
  end

  local ts = os.date "%Y-%m-%d %H:%M:%S"
  local prefix = ("[%s] [%s] "):format(ts, source)
  local normalized = text:gsub("\r\n", "\n")
  local out = {}

  for line in (normalized .. "\n"):gmatch "(.-)\n" do
    out[#out + 1] = prefix .. line .. "\n"
  end
  write_raw(table.concat(out))
end

return M
