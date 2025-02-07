defmodule Exile do
  @moduledoc ~S"""
  Exile is an alternative for beam [ports](https://hexdocs.pm/elixir/Port.html)
  with back-pressure and non-blocking IO.

  ### Quick Start

  Run a command and read from stdout

  ```
  iex> Exile.stream!(~w(echo Hello))
  ...> |> Enum.into("") # collect as string
  "Hello\n"
  ```

  Run a command with list of strings as input

  ```
  iex> Exile.stream!(~w(cat), input: ["Hello", " ", "World"])
  ...> |> Enum.into("") # collect as string
  "Hello World"
  ```

  Run a command with input as Stream

  ```
  iex> input_stream = Stream.map(1..10, fn num -> "#{num} " end)
  iex> Exile.stream!(~w(cat), input: input_stream)
  ...> |> Enum.into("")
  "1 2 3 4 5 6 7 8 9 10 "
  ```

  Run a command with input as infinite stream

  ```
  # create infinite stream
  iex> input_stream = Stream.repeatedly(fn -> "A" end)
  iex> binary =
  ...>   Exile.stream!(~w(cat), input: input_stream, ignore_epipe: true) # we need to ignore epipe since we are terminating the program before the input completes
  ...>   |> Stream.take(2) # we must limit since the input stream is infinite
  ...>   |> Enum.into("")
  iex> is_binary(binary)
  true
  iex> "AAAAA" <> _ = binary
  ```

  Run a command with input Collectable

  ```
  # Exile calls the callback with a sink where the process can push the data
  iex> Exile.stream!(~w(cat), input: fn sink ->
  ...>   Stream.map(1..10, fn num -> "#{num} " end)
  ...>   |> Stream.into(sink) # push to the external process
  ...>   |> Stream.run()
  ...> end)
  ...> |> Stream.take(100) # we must limit since the input stream is infinite
  ...> |> Enum.into("")
  "1 2 3 4 5 6 7 8 9 10 "
  ```

  When the command wait for the input stream to close

  ```
  # base64 command wait for the input to close and writes data to stdout at once
  iex> Exile.stream!(~w(base64), input: ["abcdef"])
  ...> |> Enum.into("")
  "YWJjZGVm\n"
  ```

  When the command exit with an error

  ```
  iex> Exile.stream!(["sh", "-c", "exit 4"])
  ...> |> Enum.into("")
  ** (Exile.Process.Error) command exited with status: 4
  ```

  With `max_chunk_size` set

  ```
  iex> data =
  ...>   Exile.stream!(~w(cat /dev/urandom), max_chunk_size: 100, ignore_epipe: true)
  ...>   |> Stream.take(5)
  ...>   |> Enum.into("")
  iex> byte_size(data)
  500
  ```

  When input and output run at different rate

  ```
  iex> input_stream = Stream.map(1..1000, fn num -> "X #{num} X\n" end)
  iex> Exile.stream!(~w(grep 250), input: input_stream)
  ...> |> Enum.into("")
  "X 250 X\n"
  ```

  With stderr enabled

  ```
  iex> Exile.stream!(["sh", "-c", "echo foo\necho bar >> /dev/stderr"], enable_stderr: true)
  ...> |> Enum.to_list()
  [{:stdout, "foo\n"}, {:stderr, "bar\n"}]
  ```

  For more details about stream API, see `Exile.stream!/2`.

  For more details about inner working, please check `Exile.Process`
  documentation.
  """

  use Application

  @doc false
  def start(_type, _args) do
    opts = [
      name: Exile.WatcherSupervisor,
      strategy: :one_for_one
    ]

    # We use DynamicSupervisor for cleaning up external processes on
    # :init.stop or SIGTERM
    DynamicSupervisor.start_link(opts)
  end

  @doc ~S"""
  Runs the command with arguments and return an the stdout as lazily
  Enumerable stream, similar to [`Stream`](https://hexdocs.pm/elixir/Stream.html).

  First parameter must be a list containing command with arguments.
  Example: `["cat", "file.txt"]`.

  ### Options

    * `input` - Input can be either an `Enumerable` or a function which accepts `Collectable`.

      * Enumerable:

        ```
        # List
        Exile.stream!(~w(base64), input: ["hello", "world"]) |> Enum.to_list()
        # Stream
        Exile.stream!(~w(cat), input: File.stream!("log.txt", [], 65_536)) |> Enum.to_list()
        ```

      * Collectable:

        If the input in a function with arity 1, Exile will call that function
        with a `Collectable` as the argument. The function must *push* input to this
        collectable. Return value of the function is ignored.

        ```
        Exile.stream!(~w(cat), input: fn sink -> Enum.into(1..100, sink, &to_string/1) end)
        |> Enum.to_list()
        ```

        By defaults no input is sent to the command.

    * `exit_timeout` - Duration to wait for external program to exit after completion
  (when stream ends). Defaults to `:infinity`

    * `max_chunk_size` - Maximum size of iodata chunk emitted by the stream.
  Chunk size can be less than the `max_chunk_size` depending on the amount of
  data available to be read. Defaults to `65_535`

    * `enable_stderr` - When set to true, output stream will contain stderr data along
  with stdout. Stream data will be of the form `{:stdout, iodata}` or `{:stderr, iodata}`
  to differentiate different streams. Defaults to false. See example below

    * `ignore_epipe` - When set to true, reader can exit early without raising error.
  Typically writer gets `EPIPE` error on write when program terminate prematurely.
  With `ignore_epipe` set to true this error will be ignored. This can be used to
  match UNIX shell default behaviour. EPIPE is the error raised when the reader finishes
  the reading and close output pipe before command completes. Defaults to `false`.

  Remaining options are passed to `Exile.Process.start_link/2`

  ### Examples

  ```
  Exile.stream!(~w(ffmpeg -i pipe:0 -f mp3 pipe:1), input: File.stream!("music_video.mkv", [], 65_535))
  |> Stream.into(File.stream!("music.mp3"))
  |> Stream.run()
  ```

  Stream with stderr

  ```
  Exile.stream!(~w(ffmpeg -i pipe:0 -f mp3 pipe:1),
    input: File.stream!("music_video.mkv", [], 65_535),
    enable_stderr: true
  )
  |> Stream.transform(
    fn ->
      File.open!("music.mp3", [:write, :binary])
    end,
    fn elem, file ->
      case elem do
        {:stdout, data} ->
          # write stdout data to a file
          :ok = IO.binwrite(file, data)

        {:stderr, msg} ->
          # write stderr output to console
          :ok = IO.write(msg)
      end

      {[], file}
    end,
    fn file ->
      :ok = File.close(file)
    end
  )
  |> Stream.run()
  ```
  """
  @type collectable_func() :: (Collectable.t() -> any())

  @spec stream!(nonempty_list(String.t()),
          input: Enum.t() | collectable_func(),
          exit_timeout: timeout(),
          max_chunk_size: pos_integer()
        ) :: Exile.Stream.t()
  def stream!(cmd_with_args, opts \\ []) do
    Exile.Stream.__build__(cmd_with_args, opts)
  end
end
