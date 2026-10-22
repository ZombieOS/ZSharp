# frozen_string_literal: true

# The Ruby-facing API. Load this with require "zsharp" and explicitly export
# functions/classes; the ZVM supplies the persistent worker and callback link.
module ZSharp
  class << self
    def export(name, callable = nil, &block)
      if name.is_a?(Class) || name.is_a?(Module)
        callable = name
        name = name.name.to_s.split('::').last
      end
      name = name.to_s
      raise ArgumentError, 'Z# exports require an identifier or qualified identifier' unless valid_name?(name)
      callable ||= block
      raise ArgumentError, 'ZSharp.export needs a callable, a block, or a class/module' unless callable

      context = Thread.current[:zsharp_module_exports]
      raise RuntimeError, 'Z# exports must be registered while the module is loading' unless context
      collection = if callable.is_a?(Class)
                     context[:types]
                   elsif callable.is_a?(Module)
                     context[:modules]
                   else
                     raise ArgumentError, 'Z# function export must respond to call' unless callable.respond_to?(:call)
                     context[:functions]
                   end
      raise ArgumentError, "Duplicate Z# export #{name}" if collection.key?(name)
      collection[name] = callable
      callable
    end

    def export_type(name, type)
      raise ArgumentError, 'ZSharp.export_type requires a class' unless type.is_a?(Class)
      export(name, type)
    end

    def call(target, *arguments)
      raise ArgumentError, 'Z# callback target must not be empty' if target.to_s.strip.empty?
      connection!.callback('host.call', target: target.to_s, arguments: arguments)
    end

    def valid_name?(name)
      /\A[A-Za-z_][A-Za-z_0-9]*(?:\.[A-Za-z_][A-Za-z_0-9]*)*\z/.match?(name)
    end

    # Internal worker connection. Do not replace it in user modules.
    attr_accessor :connection

    def connection!
      connection || raise(RuntimeError, 'Z# callbacks are only available inside the ZVM worker')
    end
  end

  # A live VM-owned Z# instance. Its fields remain in the VM, not in this proxy.
  class Object
    attr_reader :reference, :type_name

    def initialize(reference, type_name)
      @reference = reference
      @type_name = type_name
    end

    def get(member)
      ZSharp.connection!.callback('host.get', reference: reference, member: member.to_s)
    end

    def set(member, value)
      ZSharp.connection!.callback('host.set', reference: reference, member: member.to_s, arguments: [value])
    end

    def invoke(member, *arguments)
      ZSharp.connection!.callback('host.invoke', reference: reference, member: member.to_s, arguments: arguments)
    end
  end
end
