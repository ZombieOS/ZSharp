# frozen_string_literal: true

require 'minitest/autorun'
require 'tmpdir'
require 'open3'
require 'rbconfig'
require 'stringio'
require_relative 'zsharp_worker'

class ZSharpRubyWorkerTest < Minitest::Test
  FIXTURE = <<~RUBY
    require 'zsharp'

    class Counter
      attr_accessor :value
      def initialize(value)
        @value = value
      end
      def add(amount)
        @value += amount
      end
      private
      def secret
        'not public'
      end
    end

    ZSharp.export(:add) { |a, b| a + b }
    ZSharp.export(:echo) { |value| value }
    ZSharp.export(:warn) { puts 'user stdout must be stderr'; 'ok' }
    ZSharp.export(:callback) { |value| ZSharp.call('Main:Main:Double', value) }
    ZSharp.export(:object_callback) { |object| object.invoke('Add', 2) }
    ZSharp.export(:background) do
      Thread.new do
        begin
          ZSharp.call('Main:Main:Double', 1)
          'incorrectly accepted'
        rescue StandardError => error
          error.message
        end
      end.value
    end
    ZSharp.export(:fail) { raise 'test failure' }
    ZSharp.export(Counter)
  RUBY

  def number(text)
    { '$number' => text }
  end

  def test_persistent_process
    Dir.mktmpdir('zsharp-ruby-worker-') do |directory|
      filename = File.join(directory, 'Module.rb')
      File.write(filename, FIXTURE)
      worker = File.join(__dir__, 'zsharp_worker.rb')
      Open3.popen3(RbConfig.ruby, worker) do |input, output, errors, child|
        stderr = Thread.new { errors.read }
        id = 0
        read = lambda do
          line = output.gets
          refute_nil line, 'worker closed its protocol unexpectedly'
          JSON.parse(line)
        end
        send_request = lambda do |operation, target: nil, reference: nil, member: nil, arguments: []|
          id += 1
          input.puts(JSON.generate('id' => id, 'op' => operation, 'module' => filename,
                                   'target' => target, 'ref' => reference, 'member' => member, 'args' => arguments))
          input.flush
          response = read.call
          assert_equal 'result', response['op']
          assert_equal id, response['id']
          response
        end
        value = lambda do |response|
          assert_equal true, response['ok'], response['error']
          response['value']
        end

        assert_equal number('9007199254741000'), value.call(send_request.call('call', target: 'add', arguments: [number('9007199254740993'), number('7')]))
        assert_equal number('0.12345678901234567890123456789'), value.call(send_request.call('call', target: 'echo', arguments: [number('0.12345678901234567890123456789')]))
        assert_equal ['a', 'b'], value.call(send_request.call('call', target: 'echo', arguments: [['a', 'b']]))
        reference = value.call(send_request.call('new', target: 'Counter', arguments: [number('10')]))['$ref']
        assert_equal number('15'), value.call(send_request.call('invoke', reference: reference, member: 'add', arguments: [number('5')]))
        value.call(send_request.call('set', reference: reference, member: 'value', arguments: [number('20')]))
        assert_equal number('20'), value.call(send_request.call('get', reference: reference, member: 'value'))
        assert_equal reference, value.call(send_request.call('call', target: 'echo', arguments: [{ '$ref' => reference }]))['$ref']
        assert_equal false, send_request.call('invoke', reference: reference, member: 'secret')['ok']
        assert_equal false, send_request.call('invoke', reference: reference, member: 'instance_eval')['ok']
        value.call(send_request.call('release', reference: reference))
        assert_equal false, send_request.call('get', reference: reference, member: 'value')['ok']
        assert_equal false, send_request.call('call', target: 'fail')['ok']
        assert_includes value.call(send_request.call('call', target: 'background')), 'thread'
        assert_equal 'ok', value.call(send_request.call('call', target: 'warn'))

        id += 1
        input.puts(JSON.generate('id' => id, 'op' => 'call', 'module' => filename,
                                 'target' => 'callback', 'args' => [number('21')]))
        input.flush
        callback = read.call
        assert_equal 'host.call', callback['op']
        assert_equal 'Main:Main:Double', callback['target']
        input.puts(JSON.generate('id' => 'nested', 'op' => 'call', 'module' => filename, 'target' => 'warn', 'args' => []))
        input.flush
        busy = read.call
        assert_equal 'nested', busy['id']
        assert_equal false, busy['ok']
        input.puts(JSON.generate('id' => callback['id'], 'op' => 'result', 'ok' => true, 'value' => number('42')))
        input.flush
        assert_equal number('42'), value.call(read.call)

        id += 1
        input.puts(JSON.generate('id' => id, 'op' => 'call', 'module' => filename,
                                 'target' => 'object_callback', 'args' => [{ '$zref' => 'z1', '$type' => 'Counter' }]))
        input.flush
        callback = read.call
        assert_equal 'host.invoke', callback['op']
        assert_equal 'z1', callback['ref']
        assert_equal 'Add', callback['member']
        input.puts(JSON.generate('id' => callback['id'], 'op' => 'result', 'ok' => true, 'value' => number('12')))
        input.flush
        assert_equal number('12'), value.call(read.call)

        input.close
        assert child.value.success?, stderr.value
        assert_includes stderr.value, 'user stdout must be stderr'
      end
    end
  end

  def test_value_guards
    worker = ZSharp::Worker.new(StringIO.new, StringIO.new)
    [Float::INFINITY, Float::NAN].each do |number|
      assert_raises(ArgumentError) { worker.encode(number) }
    end
    ['', 'NaN', '1/2', '0x10', ' 10'].each do |text|
      assert_raises(ArgumentError) { worker.decode('$number' => text) }
    end
    assert_raises(RuntimeError) { worker.callback('host.call', target: 'Main:Main:Run') }
  ensure
    ZSharp.connection = nil
  end
end
