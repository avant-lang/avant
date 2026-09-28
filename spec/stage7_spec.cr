require "./spec_helper"

describe "Stage 7" do
  it "runs bitwise, logical, and string bytes" do
    run_src(<<-AV).should eq "5\n1\n0\n97\n"
      fn main {
        puts(7 & 5)
        puts(true && false || true)
        if !false && 1 < 2 {
          puts(0)
        }
        puts("abc"[0])
      }
      AV
  end

  it "runs break, shifts, and checksum_f64" do
    out = run_src(<<-AV).strip.split('\n')
      fn main {
        n = 0
        while n < 10 {
          if n == 3 {
            break
          }
          n = n + 1
        }
        puts(n)
        puts(8 >> 1)
        puts_u(checksum_f64(1.0))
      }
      AV
    out[0].should eq "3"
    out[1].should eq "4"
    out[2].should eq "2485463956"
  end

  it "copies Int locals into spawn (D35)" do
    run_src(<<-AV).should eq "7\n"
      fn add(a: Int, b: Int): Int {
        a + b
      }
      fn main {
        x = 3
        y = 4
        h = spawn { add(x, y) }
        puts(h.join)
      }
      AV
  end

  it "rejects capturing a class across spawn" do
    expect_raises(Avant::CompileError, /cannot capture/) do
      compile(<<-AV)
        class Box {
          n: Int
          fn initialize(n: Int) {
            self.n = n
          }
        }
        fn main {
          b = Box.new(1)
          h = spawn { b.n }
          puts(h.join)
        }
        AV
    end
  end

  it "runs chr, Array.sort, exp, and fmt_float" do
    out = run_src(<<-AV).strip.split('\n')
      fn main {
        puts(chr(97) + chr(98))
        xs: Array(Int) = [3, 1, 2]
        xs.sort
        puts(xs[0])
        puts(xs[2])
        puts(fmt_float(1.0, 7))
        puts(exp(0.0))
      }
      AV
    out[0].should eq "ab"
    out[1].should eq "1"
    out[2].should eq "3"
    out[3].should eq "1.0000000"
    out[4].should eq "1"
  end

  it "walks a yyjson array of floats" do
    run_src(<<-AV).should eq "6\n"
      fn main {
        doc = json_parse("{\\"coordinates\\":[{\\"x\\":1.0,\\"y\\":2.0,\\"z\\":3.0}]}")
        root = json_root(doc)
        if coords = json_get(root, "coordinates") {
          if c = json_at(coords, 0) {
            if x = json_f64(c, "x") {
              if y = json_f64(c, "y") {
                if z = json_f64(c, "z") {
                  puts((x + y + z).to_i)
                }
              }
            }
          }
        }
      }
      AV
  end

  it "uses Hash(Int, Int) and Array.pop" do
    run_src(<<-AV).should eq "2\n2\n2\n"
      fn main {
        h = Hash(Int, Int).new
        h[10] = 1
        if v = h.get(10) {
          h[10] = v + 1
        }
        h[20] = 3
        puts(h.size)
        if v = h.get(10) {
          puts(v)
        }
        xs: Array(Int) = [1, 2]
        puts(xs.pop)
      }
      AV
  end

  it "breaks only the inner while" do
    run_src(<<-AV).should eq "5\n"
      fn main {
        n = 0
        while n < 5 {
          k = 0
          while k < 3 {
            if k == 1 {
              break
            }
            k = k + 1
          }
          n = n + 1
        }
        puts(n)
      }
      AV
  end

  it "parses integers from strings" do
    run_src(<<-AV).should eq "42\n"
      fn main {
        puts("42".to_i)
      }
      AV
  end

  it "keeps while-body typed binds from being struct literals" do
    run_src(<<-AV).should eq "2\n"
      fn main {
        n = 2
        i = 0
        while i < n {
          xs: Array(Int) = []
          xs.push(i)
          i = i + 1
        }
        puts(i)
      }
      AV
  end

  it "builds strings from bytes in linear helpers" do
    run_src(<<-AV).should eq "aaa\nabc\nbc\n"
      fn main {
        puts(repeat_byte(97, 3))
        xs: Array(Int) = [97, 98, 99]
        puts(bytes_to_str(xs))
        puts(str_slice("abcd", 1, 3))
      }
      AV
  end

  it "wraps 64-bit add through wide_op" do
    run_src(<<-AV).should eq "0\n1\n"
      fn main {
        lo = wide_op(0, 0 - 1, 0, 1, 0)
        puts(lo)
        puts(wide_hi())
      }
      AV
  end

  it "builds strings with Buf without Array(Int)" do
    run_src(<<-AV).should eq "ab12\n3.14\n"
      fn main {
        b = buf_new()
        b.push_byte(97)
        b.push_str("b")
        b.push_fmt_i(12)
        puts(b.to_s)
        c = buf_new(8)
        c.push_fmt_f(3.14159, 2)
        puts(c.to_s)
      }
      AV
  end

  it "parses a float from a string slice" do
    run_src(<<-AV).should eq "12\n"
      fn main {
        puts(str_to_f_slice("x=12.9,", 2, 6).to_i)
      }
      AV
  end

  it "interns hash keys from slices" do
    run_src(<<-AV).should eq "1\n2\n1\nab\n"
      fn main {
        h = Hash(String, Int).new
        puts(h.inc_slice("ab ab", 0, 2))
        puts(h.inc_slice("ab ab", 3, 5))
        puts(h.size)
        puts(hash_last_key())
      }
      AV
  end

  it "encodes and decodes base64" do
    run_src(<<-AV).should eq "YWFh\naaa\n"
      fn main {
        e = b64_encode("aaa")
        puts(e)
        puts(b64_decode(e))
      }
      AV
  end

  it "encodes base64 into a reused Buf" do
    run_src(<<-AV).should eq "YWFh\naaa\n"
      fn main {
        b = buf_new(8)
        b64_encode_buf(b, "aaa")
        puts(b.to_s)
        d = buf_new(8)
        b64_decode_buf(d, b.to_s)
        puts(d.to_s)
      }
      AV
  end

  it "counts with PCRE2" do
    run_src(<<-AV).should eq "2\n"
      fn main {
        puts(re_count("a+", "aa ba", 0))
      }
      AV
  end

  it "passes Buf into a helper" do
    run_src(<<-AV).should eq "hi\n"
      fn add(b: Buf, s: String) {
        b.push_str(s)
      }
      fn main {
        b = buf_new()
        add(b, "hi")
        puts(b.to_s)
      }
      AV
  end

  it "keeps interned chr bytes alive across collections" do
    run_src(<<-AV).should eq "A\nA\n"
      fn main {
        puts(chr(65))
        i = 0
        while i < 80 {
          xs = Array(Int).new(100000)
          i = i + 1
        }
        puts(chr(65))
      }
      AV
  end

  it "uses native Int64 and UInt64" do
    out = run_src(<<-AV).strip.split('\n')
      fn main {
        a: Int64 = 1
        b = 2i64
        puts(a + b)
        u = 0xFFFFFFFFu64
        if (u + 1u64) == 4294967296u64 {
          puts(1)
        } else {
          puts(0)
        }
        xs = Array(Int).new(4)
        xs.fill(7)
        puts(xs[3])
        h = Hash(String, Int).new
        h["ab"] = 9
        if v = h.get_concat("a", "b") {
          puts(v)
        }
        acc: UInt64 = 3000000000
        puts("${acc}")
      }
      AV
    out[0].should eq "3"
    out[1].should eq "1"
    out[2].should eq "7"
    out[3].should eq "9"
    out[4].should eq "3000000000"
  end

  it "roundtrips zlib and hashes with SHA-256" do
    out = run_src(<<-AV).strip.split('\n')
      fn main {
        puts(sha256("abc"))
        puts(zlib_uncompress(zlib_compress("hello")))
      }
      AV
    out[0].should eq "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    out[1].should eq "hello"
  end

  it "clears and reserves Array(Int) then pushes without losing values" do
    run_src(<<-AV).should eq "2\n7\n9\n"
      fn main {
        xs: Array(Int) = [1, 2, 3]
        xs.clear
        xs.reserve(8)
        xs.push(7)
        xs.push(9)
        puts(xs.size)
        puts(xs[0])
        puts(xs[1])
      }
      AV
  end

  it "sums JSON object fields from an array in C" do
    run_src(<<-AV).should eq "6\n"
      fn main {
        doc = json_parse("{\\"coordinates\\":[{\\"x\\":1.0,\\"y\\":2.0,\\"z\\":3.0}]}")
        root = json_root(doc)
        if coords = json_get(root, "coordinates") {
          puts(json_sum_f64(coords, "x").to_i + json_sum_f64(coords, "y").to_i + json_sum_f64(coords, "z").to_i)
        }
      }
      AV
  end
end
