local utilities = {}

function utilities.greeting(name)
  return "Hello from Lua, " .. name .. "!"
end

function utilities.answer()
  return 42
end

function utilities.available()
  return true
end

function utilities.first_decorated(values)
  return "[" .. values[1] .. "]"
end

function utilities.number_values()
  return { 7, 11, 42 }
end

function utilities.third_number(values)
  return values[3]
end

function utilities.unicode()
  return "• — café 日本語 😀 ❤️"
end

function utilities.empty_value()
  return nil
end

function utilities.fail_clearly()
  error("intentional Lua regression failure")
end

return utilities
