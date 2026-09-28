module Avant
  module Runtime
    DIR = File.expand_path("../../runtime", __DIR__)
    SOURCE = File.join(DIR, "avant_rt.c")
    HEADER = File.join(DIR, "avant_rt.h")
    BUILD = File.join(DIR, "build")

    ARRAY_TYPE_ID = 0
    ARRAY_PTR_TYPE_ID = 65535
    ARRAY_OBJ_TYPE_ID = 1022
    HASH_TYPE_ID = 1023
    BUF_TYPE_ID = 1021
    CLASS_TYPE_BASE = 1

    RESERVED_SYMBOLS = %w(
      main
      avant_alloc
      avant_pin
      avant_gc_enter
      avant_gc_leave
      avant_gc_root
      avant_type_map
      avant_barrier
      avant_str_concat
      avant_str_from_i32_bytes
      avant_str_repeat_byte
      avant_str_slice
      avant_str_from_int
      avant_str_from_i64
      avant_str_from_u64
      avant_str_from_float
      avant_str_from_bool
      avant_str_from_byte
      avant_str_fmt_float
      avant_str_eq
      avant_str_size
      avant_str_byte
      avant_str_to_f
      avant_str_to_f_slice
      avant_str_to_i
      avant_checksum_str
      avant_checksum_f64
      avant_wide_op
      avant_wide_hi
      avant_exp
      avant_buf_new
      avant_buf_push_byte
      avant_buf_push_str
      avant_buf_push_slice
      avant_buf_push_fmt_f
      avant_buf_push_fmt_i
      avant_buf_to_str
      avant_buf_size
      avant_buf_clear
      avant_buf_starts
      avant_buf_ensure
      avant_b64_encode
      avant_b64_decode
      avant_b64_encode_buf
      avant_b64_decode_buf
      avant_crc32_i32
      avant_sha256_word0
      avant_zlib_compress
      avant_zlib_uncompress
      avant_sha256
      avant_re_compile
      avant_re_find
      avant_re_m0
      avant_re_m1
      avant_re_c0
      avant_re_c1
      avant_re_count
      avant_hash_new
      avant_hash_size
      avant_hash_set_i32
      avant_hash_set_str
      avant_hash_set_i32k_i32
      avant_hash_get_i32
      avant_hash_get_str
      avant_hash_get_i32k_i32
      avant_hash_del
      avant_hash_del_i32k
      avant_hash_inc_slice
      avant_hash_last_key
      avant_hash_get_str_slice
      avant_hash_get_concat
      avant_array_push_slot
      avant_array_push_i32
      avant_array_clear
      avant_array_reserve
      avant_array_pop_slot
      avant_array_sort_i32
      avant_array_fill_i32
      avant_spawn
      avant_join
      avant_now_ms
      avant_now_us
      avant_io_init_argv
      avant_argv
      avant_file_read
      avant_file_write
      avant_process_run
      avant_process_run_out
      avant_env_get
      avant_file_exists
      avant_dir_list
      avant_cov_init
      avant_cov_hit
      avant_json_parse
      avant_json_free
      avant_json_gen_body
      avant_json_gen_into
      avant_json_get_int
      avant_json_get_str
      avant_json_root
      avant_json_obj_get
      avant_json_arr_len
      avant_json_arr_get
      avant_json_as_f64
      avant_json_obj_f64
      avant_json_arr_sum_f64
      avant_http_roundtrip
    )

    USOCKETS_SRC = File.join(DIR, "third_party/uSockets/src")

    def self.reserved_symbol?(name : String) : Bool
      RESERVED_SYMBOLS.includes?(name)
    end

    def self.object_path : String
      File.join(BUILD, "avant_rt.o")
    end

    def self.ensure_object : String
      ensure_objects[0]
    end

    def self.ensure_objects : Array(String)
      units.each { |src, obj, flags| compile_unit(src, obj, flags) if stale?(src, obj) }
      units.map { |_, obj, _| obj }
    end

    def self.linker_flags : Array(String)
      ["-pthread", "-lm", "-lpcre2-8", "-lz"]
    end

    private def self.units : Array({String, String, Array(String)})
      Dir.mkdir_p(BUILD)
      list = [] of {String, String, Array(String)}
      c_flags = ["-O2", "-std=c11", "-pthread", "-I#{DIR}", "-I#{File.join(DIR, "third_party/yyjson")}", "-I#{USOCKETS_SRC}", "-DLIBUS_NO_SSL"]
      {
        "avant_rt.c"     => [] of String,
        "avant_str.c"    => [] of String,
        "avant_buf.c"    => [] of String,
        "avant_hash.c"   => [] of String,
        "avant_array.c"  => [] of String,
        "avant_task.c"   => [] of String,
        "avant_json.c"   => [] of String,
        "avant_http.c"   => [] of String,
        "avant_wide.c"   => [] of String,
        "avant_codec.c"  => [] of String,
        "avant_re.c"     => [] of String,
        "avant_io.c"     => [] of String,
        "avant_cov.c"    => [] of String,
      }.each do |name, extra|
        src = File.join(DIR, name)
        obj = File.join(BUILD, "#{File.basename(name, ".c")}.o")
        list << {src, obj, c_flags + extra}
      end
      yy = File.join(DIR, "third_party/yyjson/yyjson.c")
      list << {yy, File.join(BUILD, "yyjson.o"), c_flags}
      usockets.each do |rel|
        src = File.join(USOCKETS_SRC, rel)
        obj = File.join(BUILD, "us_#{File.basename(rel, ".c")}.o")
        list << {src, obj, c_flags}
      end
      list
    end

    private def self.usockets : Array(String)
      ["bsd.c", "context.c", "loop.c", "socket.c", "udp.c", "eventing/epoll_kqueue.c"]
    end

    private def self.stale?(src : String, obj : String) : Bool
      return true unless File.exists?(obj)
      obj_time = File.info(obj).modification_time
      return true unless File.exists?(src)
      return true if File.info(src).modification_time > obj_time
      File.exists?(HEADER) && File.info(HEADER).modification_time > obj_time
    end

    private def self.compile_unit(src : String, obj : String, flags : Array(String)) : Nil
      unless File.exists?(src)
        raise CompileError.at(Location.new(src, 1, 1, 0), "runtime source is missing")
      end
      Dir.mkdir_p(File.dirname(obj))
      cc = ENV["CC"]? || "cc"
      output = IO::Memory.new
      status = Process.run(
        cc,
        ["-c"] + flags + ["-o", obj, src],
        output: output,
        error: output
      )
      unless status.success?
        raise CompileError.at(Location.new(src, 1, 1, 0), "failed to build runtime:\n#{output}")
      end
    end
  end
end
