/*
 * line.c
 * Copyright (C) 2016 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "data-types.h"

static PyObject *
new(PyTypeObject UNUSED *type, PyObject UNUSED *args, PyObject UNUSED *kwds) {
    PyErr_SetString(PyExc_TypeError, "Line objects cannot be instantiated directly, create them using LineBuf.line()");
    return NULL;
}

static void
dealloc(Line* self) {
    if (self->needs_free) {
        PyMem_Free(self->chars);
        PyMem_Free(self->colors);
        PyMem_Free(self->decoration_fg);
        PyMem_Free(self->combining_chars);
    }
    Py_TYPE(self)->tp_free((PyObject*)self);
}

unsigned int
line_length(Line *self) {
    for (int i = self->xnum - 1; i >= 0; i++) {
        if ((self->chars[i] & CHAR_MASK) != 32) return i + 1;
    }
    return 0;
}

PyObject* 
line_text_at(char_type ch, combining_type cc) {
    PyObject *ans;
    if (cc == 0) {
        ans = PyUnicode_New(1, ch);
        if (ans == NULL) return PyErr_NoMemory();
        PyUnicode_WriteChar(ans, 0, ch);
    } else {
        Py_UCS4 cc1 = cc & CC_MASK, cc2 = cc >> 16;
        Py_UCS4 maxc = (ch > cc1) ? MAX(ch, cc2) : MAX(cc1, cc2);
        ans = PyUnicode_New(cc2 ? 3 : 2, maxc);
        if (ans == NULL) return PyErr_NoMemory();
        PyUnicode_WriteChar(ans, 0, ch);
        PyUnicode_WriteChar(ans, 1, cc1);
        if (cc2) PyUnicode_WriteChar(ans, 2, cc2);
    }

    return ans;
}

static PyObject*
text_at(Line* self, Py_ssize_t xval) {
#define text_at_doc "[x] -> Return the text in the specified cell"
    char_type ch;
    combining_type cc;

    if (xval >= self->xnum) { PyErr_SetString(PyExc_IndexError, "Column number out of bounds"); return NULL; }
    ch = self->chars[xval] & CHAR_MASK;
    cc = self->combining_chars[xval];
    return line_text_at(ch, cc);
}

static PyObject *
as_unicode(Line* self) {
    Py_ssize_t n = 0;
    Py_UCS4 *buf = PyMem_Malloc(3 * self->xnum * sizeof(Py_UCS4));
    if (buf == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    for(index_type i = 0; i < self->xnum; i++) {
        char_type attrs = self->chars[i] >> ATTRS_SHIFT;
        if ((attrs & WIDTH_MASK) < 1) continue;
        buf[n++] = self->chars[i] & CHAR_MASK;
        char_type cc = self->combining_chars[i];
        Py_UCS4 cc1 = cc & CC_MASK, cc2;
        if (cc1) {
            buf[n++] = cc1;
            cc2 = cc >> 16;
            if (cc2) buf[n++] = cc2;
        }
    }
    PyObject *ans = PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, buf, n);
    PyMem_Free(buf);
    return ans;
}

static PyObject *
as_base_text(Line* self) {
#define as_base_text_doc "Return the line as a string, discarding all combining characters"
    PyObject *ans = PyUnicode_New(self->xnum, 1114111);
    if (ans == NULL) return PyErr_NoMemory();
    for(index_type i = 0; i < self->xnum; i++) {
        char_type attrs = self->chars[i] >> ATTRS_SHIFT;
        if ((attrs & WIDTH_MASK) < 1) continue;
        PyUnicode_WRITE(PyUnicode_4BYTE_KIND, PyUnicode_DATA(ans), i, self->chars[i] & CHAR_MASK);
    }
    return ans;
}

static inline bool
write_sgr(unsigned int val, Py_UCS4 *buf, index_type buflen, index_type *i) {
    static char s[20] = {0};
    unsigned int num = snprintf(s, 20, "\x1b[%um", val);
    if (buflen - (*i) < num + 3) return false;
    for(unsigned int si=0; si < num; si++) buf[(*i)++] = s[si];
    return true;
}

static inline bool
write_color(uint32_t val, int code, Py_UCS4 *buf, index_type buflen, index_type *i) {
    static char s[50] = {0};
    unsigned int num;
    switch(val & 3) {
        case 1:
            num = snprintf(s, 50, "\x1b[%d;5;%um", code, (val >> 8) & 0xFF); break;
        case 2:
            num = snprintf(s, 50, "\x1b[%d;2;%u;%u;%um", code, (val >> 24) & 0xFF, (val >> 16) & 0xFF, (val >> 8) & 0xFF); break;
        default:
            return true;
    }
    if (buflen - (*i) < num + 3) return false;
    for(unsigned int si=0; si < num; si++) buf[(*i)++] = s[si];
    return true;
}

index_type
line_as_ansi(Line *self, Py_UCS4 *buf, index_type buflen) {
#define WRITE_SGR(val) if (!write_sgr(val, buf, buflen, &i)) return i;
#define WRITE_COLOR(val, code) if (val) { if (!write_color(val, code, buf, buflen, &i)) return i; } else { WRITE_SGR(code+1); }
#define CHECK_BOOL(name, shift, on, off) \
        if (((attrs >> shift) & 1) != name) { \
            name ^= 1; \
            if (name) { WRITE_SGR(on); } else { WRITE_SGR(off); } \
        }
#define CHECK_COLOR(name, val, off_code) if (name != (val)) { name = (val); WRITE_COLOR(name, off_code); }
#define WRITE_CH(val) if (i > buflen - 1) return i; buf[i++] = val; 

    index_type limit = self->xnum, i=0;
    int r;
    if (!self->continued) {  // Trim trailing spaces
        for(r = self->xnum - 1; r >= 0; r--) {
            if ((self->chars[r] & CHAR_MASK) != 32) break;
        }
        limit = r + 1;
    }
    bool bold = false, italic = false, reverse = false, strike = false;
    uint32_t fg = 0, bg = 0, decoration_fg = 0, decoration = 0;

    WRITE_SGR(0);
    for (index_type pos=0; pos < limit; pos++) {
        char_type attrs = self->chars[pos] >> ATTRS_SHIFT, ch = self->chars[pos] & CHAR_MASK;
        if (ch == 0 || (attrs & WIDTH_MASK) < 1) continue;
        CHECK_BOOL(bold, BOLD_SHIFT, 1, 22);
        CHECK_BOOL(italic, ITALIC_SHIFT, 3, 23);
        CHECK_BOOL(reverse, REVERSE_SHIFT, 7, 27);
        CHECK_BOOL(strike, STRIKE_SHIFT, 9, 29);
        if (((attrs >> DECORATION_SHIFT) & DECORATION_MASK) != decoration) {
            decoration = ((attrs >> DECORATION_SHIFT) & DECORATION_MASK);
            switch(decoration) {
                case 1:
                    WRITE_SGR(4); break;
                case 2:
                    WRITE_SGR(UNDERCURL_CODE); break;
                default:
                    WRITE_SGR(0); break;
            }
        }
        color_type col = self->colors[pos];
        CHECK_COLOR(fg, col & COL_MASK, 38);
        CHECK_COLOR(bg, col >> COL_SHIFT, 48);
        CHECK_COLOR(decoration_fg, self->decoration_fg[pos], DECORATION_FG_CODE);
        WRITE_CH(ch);
        char_type cc = self->combining_chars[pos];
        Py_UCS4 cc1 = cc & CC_MASK;
        if (cc1) {
            WRITE_CH(cc1);
            cc1 = cc >> 16;
            if (cc1) { WRITE_CH(cc1); }
        }
    }
    return i;
#undef CHECK_BOOL
#undef CHECK_COLOR
#undef WRITE_SGR
#undef WRITE_CH
#undef WRITE_COLOR
}

static PyObject*
as_ansi(Line* self) {
#define as_ansi_doc "Return the line's contents with ANSI (SGR) escape codes for formatting"
    static Py_UCS4 t[5120] = {0};
    index_type num = line_as_ansi(self, t, 5120);
    PyObject *ans = PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, t, num);
    return ans;
}

static PyObject*
__repr__(Line* self) {
    PyObject *s = as_unicode(self);
    if (s == NULL) return NULL;
    PyObject *ans = PyObject_Repr(s);
    Py_CLEAR(s);
    return ans;
}

static PyObject*
width(Line *self, PyObject *val) {
#define width_doc "width(x) -> the width of the character at x"
    unsigned long x = PyLong_AsUnsignedLong(val);
    if (x >= self->xnum) { PyErr_SetString(PyExc_ValueError, "Out of bounds"); return NULL; }
    char_type attrs = self->chars[x] >> ATTRS_SHIFT;
    return PyLong_FromUnsignedLong((unsigned long) (attrs & WIDTH_MASK));
}

static PyObject*
basic_cell_data(Line *self, PyObject *val) {
#define basic_cell_data_doc "basic_cell_data(x) -> ch, attrs, colors"
    unsigned long x = PyLong_AsUnsignedLong(val);
    if (x >= self->xnum) { PyErr_SetString(PyExc_ValueError, "Out of bounds"); return NULL; }
    char_type ch = self->chars[x];
    return Py_BuildValue("IBK", (unsigned int)(ch & CHAR_MASK), (unsigned char)(ch >> ATTRS_SHIFT), (unsigned long long)self->colors[x]);
}

void line_add_combining_char(Line *self, uint32_t ch, unsigned int x) {
    combining_type c = self->combining_chars[x];
    if (c & CC_MASK) self->combining_chars[x] = (c & CC_MASK) | ( (ch & CC_MASK) << CC_SHIFT );
    else self->combining_chars[x] = ch & CC_MASK;
}

static PyObject*
add_combining_char(Line* self, PyObject *args) {
#define add_combining_char_doc "add_combining_char(x, ch) -> Add the specified character as a combining char to the specified cell."
    int new_char;
    unsigned int x;
    if (!PyArg_ParseTuple(args, "IC", &x, &new_char)) return NULL;
    if (x >= self->xnum) {
        PyErr_SetString(PyExc_ValueError, "Column index out of bounds");
        return NULL;
    }
    line_add_combining_char(self, new_char, x);
    Py_RETURN_NONE;
}


static PyObject*
set_text(Line* self, PyObject *args) {
#define set_text_doc "set_text(src, offset, sz, cursor) -> Set the characters and attributes from the specified text and cursor"
    PyObject *src;
    Py_ssize_t offset, sz, limit;
    char_type attrs;
    Cursor *cursor;
    int kind;
    void *buf;

    if (!PyArg_ParseTuple(args, "UnnO!", &src, &offset, &sz, &Cursor_Type, &cursor)) return NULL;
    if (PyUnicode_READY(src) != 0) {
        PyErr_NoMemory();
        return NULL;
    }
    kind = PyUnicode_KIND(src);
    buf = PyUnicode_DATA(src);
    limit = offset + sz;
    if (PyUnicode_GET_LENGTH(src) < limit) {
        PyErr_SetString(PyExc_ValueError, "Out of bounds offset/sz");
        return NULL;
    }
    attrs = CURSOR_TO_ATTRS(cursor, 1);
    color_type col = (cursor->fg & COL_MASK) | ((color_type)(cursor->bg & COL_MASK) << COL_SHIFT);
    decoration_type dfg = cursor->decoration_fg & COL_MASK;

    for (index_type i = cursor->x; offset < limit && i < self->xnum; i++, offset++) {
        self->chars[i] = (PyUnicode_READ(kind, buf, offset) & CHAR_MASK) | attrs;
        self->colors[i] = col;
        self->decoration_fg[i] = dfg;
        self->combining_chars[i] = 0;
    }

    Py_RETURN_NONE;
}

static PyObject*
cursor_from(Line* self, PyObject *args) {
#define cursor_from_doc "cursor_from(x, y=0) -> Create a cursor object based on the formatting attributes at the specified x position. The y value of the cursor is set as specified."
    unsigned int x, y = 0;
    Cursor* ans;
    if (!PyArg_ParseTuple(args, "I|I", &x, &y)) return NULL;
    if (x >= self->xnum) {
        PyErr_SetString(PyExc_ValueError, "Out of bounds x");
        return NULL;
    }
    ans = alloc_cursor();
    if (ans == NULL) { PyErr_NoMemory(); return NULL; }
    ans->x = x; ans->y = y;
    char_type attrs = self->chars[x] >> ATTRS_SHIFT;
    ATTRS_TO_CURSOR(attrs, ans);
    COLORS_TO_CURSOR(self->colors[x], ans);
    ans->decoration_fg = self->decoration_fg[x] & COL_MASK;

    return (PyObject*)ans;
}

void line_clear_text(Line *self, unsigned int at, unsigned int num, int ch) {
    const char_type repl = ((char_type)ch & CHAR_MASK) | (1 << ATTRS_SHIFT);
    for (index_type i = at; i < MIN(self->xnum, at + num); i++) {
        self->chars[i] = (self->chars[i] & ATTRS_MASK_WITHOUT_WIDTH) | repl;
    }
    memset(self->combining_chars + at, 0, MIN(num, self->xnum - at) * sizeof(combining_type));
}

static PyObject*
clear_text(Line* self, PyObject *args) {
#define clear_text_doc "clear_text(at, num, ch=' ') -> Clear characters in the specified range, preserving formatting."
    unsigned int at, num;
    int ch = 32;
    if (!PyArg_ParseTuple(args, "II|C", &at, &num, &ch)) return NULL;
    line_clear_text(self, at, num, ch);
    Py_RETURN_NONE;
}

void line_apply_cursor(Line *self, Cursor *cursor, unsigned int at, unsigned int num, bool clear_char) {
    char_type attrs = CURSOR_TO_ATTRS(cursor, 1);
    color_type col = (cursor->fg & COL_MASK) | ((color_type)(cursor->bg & COL_MASK) << COL_SHIFT);
    decoration_type dfg = cursor->decoration_fg & COL_MASK;
    if (!clear_char) attrs = ((attrs >> ATTRS_SHIFT) & ~WIDTH_MASK) << ATTRS_SHIFT;
    
    for (index_type i = at; i < self->xnum && i < at + num; i++) {
        if (clear_char) {
            self->chars[i] = 32 | attrs;
            self->combining_chars[i] = 0;
        } else {
            char_type w = ((self->chars[i] >> ATTRS_SHIFT) & WIDTH_MASK) << ATTRS_SHIFT;
            self->chars[i] = (self->chars[i] & CHAR_MASK) | attrs | w;
        }
        self->colors[i] = col;
        self->decoration_fg[i] = dfg;
    }
}

static PyObject*
apply_cursor(Line* self, PyObject *args) {
#define apply_cursor_doc "apply_cursor(cursor, at=0, num=1, clear_char=False) -> Apply the formatting attributes from cursor to the specified characters in this line."
    Cursor* cursor;
    unsigned int at=0, num=1;
    int clear_char = 0;
    if (!PyArg_ParseTuple(args, "O!|IIp", &Cursor_Type, &cursor, &at, &num, &clear_char)) return NULL;
    line_apply_cursor(self, cursor, at, num, clear_char & 1);
    Py_RETURN_NONE;
}

void line_right_shift(Line *self, unsigned int at, unsigned int num) {
    for(index_type i = self->xnum - 1; i >= at + num; i--) {
        COPY_SELF_CELL(i - num, i)
    }
    // Check if a wide character was split at the right edge
    char_type w = (self->chars[self->xnum - 1] >> ATTRS_SHIFT) & 3;
    if (w != 1) self->chars[self->xnum - 1] = (1 << ATTRS_SHIFT) | 32;
}

static PyObject*
right_shift(Line *self, PyObject *args) {
#define right_shift_doc "right_shift(at, num) -> ..."
    unsigned int at, num;
    if (!PyArg_ParseTuple(args, "II", &at, &num)) return NULL;
    if (at >= self->xnum || at + num > self->xnum) {
        PyErr_SetString(PyExc_ValueError, "Out of bounds");
        return NULL;
    }
    if (num > 0) {
        line_right_shift(self, at, num);
    }
    Py_RETURN_NONE;
}

static PyObject*
left_shift(Line *self, PyObject *args) {
#define left_shift_doc "left_shift(at, num) -> ..."
    unsigned int at, num;
    if (!PyArg_ParseTuple(args, "II", &at, &num)) return NULL;
    if (at >= self->xnum || at + num > self->xnum) {
        PyErr_SetString(PyExc_ValueError, "Out of bounds");
        return NULL;
    }
    if (num > 0) left_shift_line(self, at, num);
    Py_RETURN_NONE;
}
 
void line_set_char(Line *self, unsigned int at, uint32_t ch, unsigned int width, Cursor *cursor) {
    char_type attrs;
    if (cursor == NULL) {
        attrs = (((self->chars[at] >> ATTRS_SHIFT) & ~3) | (width & 3)) << ATTRS_SHIFT;
    } else {
        attrs = CURSOR_TO_ATTRS(cursor, width & 3);
        self->colors[at] = (cursor->fg & COL_MASK) | ((color_type)(cursor->bg & COL_MASK) << COL_SHIFT);
        self->decoration_fg[at] = cursor->decoration_fg & COL_MASK;
    }
    self->chars[at] = (ch & CHAR_MASK) | attrs;
    self->combining_chars[at] = 0;
}

static PyObject*
set_char(Line *self, PyObject *args) {
#define set_char_doc "set_char(at, ch, width=1, cursor=None) -> Set the character at the specified cell. If cursor is not None, also set attributes from that cursor."
    unsigned int at, width=1;
    int ch;
    Cursor *cursor = NULL;

    if (!PyArg_ParseTuple(args, "IC|IO!", &at, &ch, &width, &Cursor_Type, &cursor)) return NULL;
    if (at >= self->xnum) {
        PyErr_SetString(PyExc_ValueError, "Out of bounds");
        return NULL;
    }
    line_set_char(self, at, ch, width, cursor);
    Py_RETURN_NONE;
}

static PyObject*
set_attribute(Line *self, PyObject *args) {
#define set_attribute_doc "set_attribute(which, val) -> Set the attribute on all cells in the line."
    unsigned int shift, val;
    char_type mask;
    if (!PyArg_ParseTuple(args, "II", &shift, &val)) return NULL;
    if (shift < DECORATION_SHIFT || shift > STRIKE_SHIFT) { PyErr_SetString(PyExc_ValueError, "Unknown attribute"); return NULL; }
    SET_ATTRIBUTE(self->chars, shift, val);
    Py_RETURN_NONE;
}

static Py_ssize_t
__len__(PyObject *self) {
    return (Py_ssize_t)(((Line*)self)->ynum);
}

static int __eq__(Line *a, Line *b) {
    return a->xnum == b->xnum && \
                    memcmp(a->chars, b->chars, sizeof(char_type) * a->xnum) == 0 && \
                    memcmp(a->colors, b->colors, sizeof(color_type) * a->xnum) == 0 && \
                    memcmp(a->decoration_fg, b->decoration_fg, sizeof(decoration_type) * a->xnum) == 0 && \
                    memcmp(a->combining_chars, b->combining_chars, sizeof(combining_type) * a->xnum) == 0;
}

// Boilerplate {{{
static PyObject*
copy_char(Line* self, PyObject *args);
#define copy_char_doc "copy_char(src, to, dest) -> Copy the character at src to to the character dest in the line `to`"

static PyObject *
richcmp(PyObject *obj1, PyObject *obj2, int op);


static PySequenceMethods sequence_methods = {
    .sq_length = __len__,                  
    .sq_item = (ssizeargfunc)text_at
};

static PyMethodDef methods[] = {
    METHOD(add_combining_char, METH_VARARGS)
    METHOD(set_text, METH_VARARGS)
    METHOD(cursor_from, METH_VARARGS)
    METHOD(apply_cursor, METH_VARARGS)
    METHOD(clear_text, METH_VARARGS)
    METHOD(copy_char, METH_VARARGS)
    METHOD(right_shift, METH_VARARGS)
    METHOD(left_shift, METH_VARARGS)
    METHOD(set_char, METH_VARARGS)
    METHOD(set_attribute, METH_VARARGS)
    METHOD(as_base_text, METH_NOARGS)
    METHOD(as_ansi, METH_NOARGS)
    METHOD(width, METH_O)
    METHOD(basic_cell_data, METH_O)
        
    {NULL}  /* Sentinel */
};

PyTypeObject Line_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "fast_data_types.Line",
    .tp_basicsize = sizeof(Line),
    .tp_dealloc = (destructor)dealloc,
    .tp_repr = (reprfunc)__repr__,
    .tp_str = (reprfunc)as_unicode,
    .tp_as_sequence = &sequence_methods,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_richcompare = richcmp,                   
    .tp_doc = "Lines",
    .tp_methods = methods,
    .tp_new = new
};

Line *alloc_line() {
    Line *ans = (Line*)PyType_GenericAlloc(&Line_Type, 0);
    ans->needs_free = 0;
    return ans;
}

RICHCMP(Line)
// }}
 
static PyObject*
copy_char(Line* self, PyObject *args) {
    unsigned int src, dest;
    Line *to;
    if (!PyArg_ParseTuple(args, "IO!I", &src, &Line_Type, &to, &dest)) return NULL;
    if (src >= self->xnum || dest >= to->xnum) {
        PyErr_SetString(PyExc_ValueError, "Out of bounds");
        return NULL;
    }
    to->chars[dest] = self->chars[src];
    to->colors[dest] = self->colors[src];
    to->decoration_fg[dest] = self->decoration_fg[src];
    to->combining_chars[dest] = self->combining_chars[src];
    Py_RETURN_NONE;
}

INIT_TYPE(Line)
