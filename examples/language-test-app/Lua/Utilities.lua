local utilities = {}

function utilities.greet(name)
  if name == "" then
    name = "Z# developer"
  end
  return "Hello from Lua, " .. name .. "!"
end

function utilities.add(left, right)
  return left + right
end

function utilities.is_ready()
  return true
end

return utilities
