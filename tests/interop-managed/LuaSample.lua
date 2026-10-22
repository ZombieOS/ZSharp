local ZSharp = require("zsharp")
local count = 0
local Counter = {}
Counter.__index = Counter
function Counter:add(amount) self.value = self.value + amount; return self.value end
function Counter:self() return self end

return {
    counted = function() count = count + 1; return count end,
    echo = function(value) return value end,
    Counter = function(value) return setmetatable({value = value}, Counter) end,
    Empty = function() return {} end,
    record = function() return ZSharp.record({name = "bridge", value = 42}) end,
    callback = function(value) return ZSharp.call("Tests:Bridge:Double", value) end,
    proxy = function(object)
        local initial = object:get("value")
        object:set("value", initial + 3)
        return object:invoke("add", 7)
    end,
    reentrant = function() return ZSharp.call("Tests:Bridge:Reentrant") end,
    nonfinite = function() return {0 / 0} end,
    timeout = function() while true do end end,
    fail = function() error("intentional Lua failure") end,
}
