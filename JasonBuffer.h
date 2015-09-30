#ifndef JASON_BUFFER_H
#define JASON_BUFFER_H 1

#include <cstring>
#include "Jason.h"

namespace triagens {
  namespace basics {

    class JasonBuffer {
        JasonBuffer (JasonBuffer const&) = delete;
        JasonBuffer& operator= (JasonBuffer const&) = delete;

      public:

        JasonBuffer () : _buf(_local), _alloc(sizeof(_local)), _pos(0) {
        } 

        explicit JasonBuffer (JasonLength expectedLength) : JasonBuffer() {
          reserve(expectedLength);
        }

        ~JasonBuffer () {
          if (_buf != nullptr && _buf != _local) {
            delete[] _buf;
          }
        }

        char const* data () const {
          return _buf;
        }

        JasonLength size () const {
          return _pos;
        }

        void push_back (char c) {
          reserve(_pos + 1); 
          _buf[_pos++] = c;
        }

        void append (char c) {
          reserve(_pos + 1); 
          _buf[_pos++] = c;
        }

        void append (char const* p, JasonLength len) {
          reserve(_pos + len);
          memcpy(_buf + _pos, p, len);
          _pos += len;
        }

        void append (uint8_t const* p, JasonLength len) {
          reserve(_pos + len);
          memcpy(_buf + _pos, p, len);
          _pos += len;
        }

        void reserve (JasonLength);
       
      private:
 
        char*       _buf;
        JasonLength _alloc;
        JasonLength _pos;

        char        _local[160];

        static double const GrowthFactor;

    };

  }  // namespace triagens::basics
}  // namespace triagens

#endif
