/* Stage 29 W5a1 (O393): C enums for avant bind (tests/cases/test_bind.av). */
typedef enum { RED, GREEN = 5, BLUE } color_t;
enum http_method { HTTP_GET, HTTP_HEAD, HTTP_POST, HTTP_ALIAS = 1 };
enum { FLAG_A = 1, FLAG_B = 2 };
enum big { BIG_ONE = 1, BIG_HUGE = 3000000000u };
enum clash { CL_A, cl_a };
enum word { WORD_A, IF };
struct req {
  enum http_method method;
  color_t color;
  int n;
};
color_t next_color(color_t c);
int method_code(enum http_method m);
enum http_method method_of(int raw);
color_t color_of(int raw);
unsigned int big_value(enum big b);
int clash_value(enum clash c);
struct req *req_new(void);
void req_fill(struct req *r, int raw_method);
int req_method_code(struct req *r);
