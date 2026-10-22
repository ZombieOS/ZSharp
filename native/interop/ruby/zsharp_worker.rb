# frozen_string_literal: true

require 'json'
require 'bigdecimal'
require_relative 'zsharp'
$LOAD_PATH.unshift(__dir__) unless $LOAD_PATH.include?(__dir__)

module ZSharp
  class Worker
    LIMIT = 1024 * 1024
    BUILTIN_OWNERS = [::Object, ::BasicObject, ::Kernel, ::Module, ::Class].freeze

    def initialize(input, output)
      @input = input
      @output = output
      @modules = {}
      @objects = {}
      @identities = {}
      @next_object = 0
      @next_callback = 0
      @owner = Thread.current
      @active = false
      ZSharp.connection = self
    end

    def serve
      while (line = read_line)
        id = nil
        begin
          request = parse(line)
          id = request['id']
          @active = true
          value = dispatch(request)
          respond(id, value: encode(value))
        rescue StandardError, ScriptError => error
          respond(id, error: "#{error.class}: #{error.message}")
        ensure
          @active = false
        end
      end
    ensure
      ZSharp.connection = nil
      @objects.clear
      @identities.clear
      @modules.clear
    end

    def callback(operation, target: nil, reference: nil, member: nil, arguments: [])
      unless Thread.current.equal?(@owner) && @active
        raise RuntimeError, 'Z# callbacks must run on the thread handling the current foreign call'
      end
      @next_callback += 1
      id = -@next_callback
      request = { 'id' => id, 'op' => operation, 'args' => arguments.map { |argument| encode(argument) } }
      request['target'] = target if target
      if reference
        request['ref'] = reference
        request['member'] = member
      end
      send_message(request)
      while (line = read_line)
        response = parse(line)
        if response['op'] != 'result'
          respond(response['id'], error: 'Reentrant call to the active Ruby worker is not supported')
          next
        end
        raise RuntimeError, 'Unexpected ZVM callback response ID' unless response['id'] == id
        raise RuntimeError, "Z# callback failed: #{response['error']}" unless response['ok'] == true
        return decode(response['value'])
      end
      raise RuntimeError, 'ZVM connection closed during Ruby callback'
    end

    def dispatch(request)
      operation = request['op']
      raw = request.fetch('args', [])
      raise ArgumentError, 'Ruby arguments must be an array' unless raw.is_a?(Array)
      arguments = raw.map { |argument| decode(argument) }
      if operation == 'call' || operation == 'new'
        exports = load_module(request['module'])
        target = request['target'].to_s
        if operation == 'new'
          type = exports[:types][target]
          raise ArgumentError, "Ruby class #{target.inspect} is not exported" unless type
          return type.new(*arguments)
        end
        function = exports[:functions][target]
        return function.call(*arguments) if function
        collection, separator, member = target.rpartition('.')
        type = exports[:modules][collection] || exports[:types][collection]
        if !separator.empty? && type && public_member?(type, member)
          return type.public_send(member, *arguments)
        end
        raise ArgumentError, "Ruby function #{target.inspect} is not exported"
      end

      reference = request['ref']
      raise ArgumentError, 'Ruby object reference is invalid or has been released' unless reference.is_a?(String) && @objects.key?(reference)
      object = @objects[reference]
      if operation == 'release'
        @objects.delete(reference)
        @identities.delete(object.__id__)
        return nil
      end
      member = request['member'].to_s
      raise ArgumentError, 'Ruby object member must be one identifier' unless /\A[A-Za-z_][A-Za-z_0-9]*\z/.match?(member)
      case operation
      when 'get'
        raise ArgumentError, 'Ruby field get does not accept arguments' unless arguments.empty?
        raise ArgumentError, "Ruby reader #{member.inspect} is not public" unless public_member?(object, member)
        object.public_send(member)
      when 'set'
        writer = "#{member}="
        raise ArgumentError, 'Ruby field set requires exactly one value' unless arguments.length == 1
        raise ArgumentError, "Ruby writer #{writer.inspect} is not public" unless public_member?(object, writer)
        object.public_send(writer, arguments.first)
        nil
      when 'invoke'
        raise ArgumentError, "Ruby method #{member.inspect} is not public" unless public_member?(object, member)
        object.public_send(member, *arguments)
      else
        raise ArgumentError, "Unsupported Ruby bridge operation #{operation.inspect}"
      end
    end

    def encode(value, depth = 0)
      raise ArgumentError, 'Ruby bridge value nesting limit exceeded' if depth > 128
      case value
      when nil, true, false, String
        value
      when Integer
        { '$number' => value.to_s }
      when BigDecimal
        raise ArgumentError, 'Non-finite Ruby number cannot cross the bridge' unless value.finite?
        { '$number' => value.to_s('F') }
      when Float
        raise ArgumentError, 'Non-finite Ruby number cannot cross the bridge' unless value.finite?
        { '$number' => value.to_s }
      when ZSharp::Object
        { '$zref' => value.reference, '$type' => value.type_name }
      when Array
        value.map { |item| encode(item, depth + 1) }
      when Hash
        fields = {}
        value.each do |key, item|
          raise ArgumentError, 'Ruby records require string or symbol keys' unless key.is_a?(String) || key.is_a?(Symbol)
          raise ArgumentError, 'Ruby record has duplicate text keys' if fields.key?(key.to_s)
          fields[key.to_s] = encode(item, depth + 1)
        end
        { '$record' => fields }
      else
        reference = @identities[value.__id__]
        unless reference
          @next_object += 1
          reference = @next_object.to_s
          @identities[value.__id__] = reference
          @objects[reference] = value
        end
        { '$ref' => reference, '$type' => value.class.name.to_s }
      end
    end

    def decode(value, depth = 0)
      raise ArgumentError, 'Ruby bridge value nesting limit exceeded' if depth > 128
      case value
      when Hash
        if value.key?('$number')
          text = value['$number']
          unless text.is_a?(String) && /\A[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?\z/.match?(text)
            raise ArgumentError, '$number must contain finite decimal text'
          end
          number = BigDecimal(text)
          raise ArgumentError, 'Non-finite Ruby number cannot cross the bridge' unless number.finite?
          number.frac.zero? ? number.to_i : number
        elsif value.key?('$ref')
          raise ArgumentError, 'Ruby object reference is invalid or has been released' unless @objects.key?(value['$ref'])
          @objects[value['$ref']]
        elsif value.key?('$zref')
          reference = value['$zref']
          raise ArgumentError, 'Invalid Z# object reference' unless reference.is_a?(String) && !reference.empty?
          ZSharp::Object.new(reference, value['$type'].to_s)
        elsif value.key?('$record')
          fields = value['$record']
          raise ArgumentError, '$record must contain an object' unless fields.is_a?(Hash)
          fields.to_h { |key, item| [key, decode(item, depth + 1)] }
        else
          raise ArgumentError, 'Unknown Ruby bridge value tag'
        end
      when Array
        value.map { |item| decode(item, depth + 1) }
      else
        value
      end
    end

    private

    def load_module(filename)
      raise ArgumentError, 'Ruby module path is required' unless filename.is_a?(String) && !filename.empty?
      path = File.realpath(filename)
      raise ArgumentError, 'Ruby modules must use the .rb extension' unless File.extname(path).downcase == '.rb'
      return @modules[path] if @modules.key?(path)
      exports = { functions: {}, types: {}, modules: {} }
      previous = Thread.current[:zsharp_module_exports]
      Thread.current[:zsharp_module_exports] = exports
      begin
        # Wrap each module to prevent unrelated imports reopening one another's
        # top-level constants. Explicitly registered callables retain the scope.
        Kernel.load(path, true)
      ensure
        Thread.current[:zsharp_module_exports] = previous
      end
      @modules[path] = exports
    end

    def public_member?(object, member)
      return false unless object.respond_to?(member)
      method = object.public_method(member)
      !BUILTIN_OWNERS.include?(method.owner)
    rescue NameError
      false
    end

    def read_line
      line = @input.gets(LIMIT + 1)
      raise ArgumentError, 'Ruby bridge message exceeds 1 MiB' if line && line.bytesize > LIMIT
      line
    end

    def parse(line)
      request = JSON.parse(line, max_nesting: 140)
      raise ArgumentError, 'Ruby bridge request must be an object with an operation' unless request.is_a?(Hash) && request['op'].is_a?(String)
      request
    end

    def respond(id, value: nil, error: nil)
      response = { 'id' => id, 'op' => 'result', 'ok' => error.nil? }
      response[error ? 'error' : 'value'] = error || value
      send_message(response)
    end

    def send_message(message)
      encoded = JSON.generate(message)
      raise ArgumentError, 'Ruby bridge message exceeds 1 MiB' if encoded.bytesize > LIMIT
      @output.write(encoded, "\n")
      @output.flush
    end
  end
end

if $PROGRAM_NAME == __FILE__
  protocol = STDOUT.dup
  protocol.set_encoding(Encoding::UTF_8)
  protocol.sync = true
  STDOUT.reopen(STDERR)
  $stdout = $stderr
  ZSharp::Worker.new(STDIN, protocol).serve
end
