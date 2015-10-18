#ifndef JASON_BUILDER_H
#define JASON_BUILDER_H

#include <iostream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <algorithm>

#include "Jason.h"
#include "JasonBuffer.h"
#include "JasonSlice.h"
#include "JasonType.h"

namespace arangodb {
  namespace jason {

    class JasonBuilder {

        friend class JasonParser;

      // This class organizes the buildup of a Jason object. It manages
      // the memory allocation and allows convenience methods to build
      // the object up recursively.
      //
      // Use as follows:                         to build Jason like this:
      //   JasonBuilder b;
      //   b.add(Jason(5, JasonType::Object));    b = {
      //   b.add("a", Jason(1.0));                      "a": 1.0,
      //   b.add("b", Jason());                         "b": null,
      //   b.add("c", Jason(false));                    "c": false,
      //   b.add("d", Jason("xyz"));                    "d": "xyz",
      //   b.add("e", Jason(3, JasonType::Array));      "e": [
      //   b.add(Jason(2.3));                                   2.3,
      //   b.add(Jason("abc"));                                 "abc",
      //   b.add(Jason(true));                                  true
      //   b.close();                                         ],
      //   b.add("f", Jason(2, JasonType::Object));     "f": {
      //   b.add("hans", Jason("Wurst"));                       "hans": "wurst",
      //   b.add("hallo", Jason(3.141));                        "hallo": 3.141
      //   b.close();                                        }
      //
      // Or, if you like fancy syntactic sugar:
      //   JasonBuilder b;
      //   b(Jason(5, JasonType::Object))        b = {
      //    ("a", Jason(1.0))                          "a": 1.0,
      //    ("b", Jason())                             "b": null,
      //    ("c", Jason(false))                        "c": false,
      //    ("d", Jason("xyz"))                        "d": "xyz",
      //    ("e", JasonType::Array, 3)                 "e": [
      //      (Jason(2.3))                                    2.3,
      //      (Jason("abc"))                                 "abc",
      //      (Jason(true))()                                true ],
      //    ("f", JasonType::Object, 2)                "f": {
      //    ("hans", Jason("Wurst"))                          "hans": "wurst",
      //    ("hallo", Jason(3.141)();                         "hallo": 3.141 }

      public:

        struct JasonBuilderError : std::exception {
          private:
            std::string _msg;
          public:
            JasonBuilderError (std::string const& msg) : _msg(msg) {
            }
            char const* what() const noexcept {
              return _msg.c_str();
            }
        };

      private:

        struct SortEntrySmall {
          int32_t  nameStartOffset;
          uint16_t nameSize;
          uint16_t offset;
        };

        struct SortEntryLarge {
          uint8_t const* nameStart;
          uint64_t nameSize;
          uint64_t offset;
        };

        // thread local vector for sorting small object attributes
        static thread_local std::vector<SortEntrySmall> SortObjectSmallEntries;
        // thread local vector for sorting large object attributes
        static thread_local std::vector<SortEntryLarge> SortObjectLargeEntries;

        JasonBuffer<uint8_t> _buffer;
        uint8_t*             _start;
        JasonLength          _size;
        JasonLength          _pos;   // the current append position, always <= _size
        bool                 _attrWritten;  // indicates that an attribute name
                                            // in an object has been written
        std::vector<JasonLength>              _stack;
        std::vector<std::vector<JasonLength>> _index;

        // Here are the mechanics of how this building process works:
        // The whole Jason being built starts at where _start points to
        // and uses at most _size bytes. The variable _pos keeps the
        // current write position. The method "set" simply writes a new
        // Jason subobject at the current write position and advances
        // it. Whenever one makes an array or object, a JasonLength for
        // the beginning of the value is pushed onto the _stack, which
        // remembers that we are in the process of building an array or
        // object. The _index vectors are used to collect information
        // for the index tables of arrays and objects, which are written
        // behind the subvalues. The add methods are used to keep track
        // of the new subvalue in _index followed by a set, and are
        // what the user from the outside calls. The close method seals
        // the innermost array or object that is currently being built
        // and pops a JasonLength off the _stack. The vectors in _index
        // stay until the next clearTemporary() is called to minimize
        // allocations. In the beginning, the _stack is empty, which
        // allows to build a sequence of unrelated Jason objects in the
        // buffer. Whenever the stack is empty, one can use the start,
        // size and stealTo methods to get out the ready built Jason
        // object(s).

        void reserveSpace (JasonLength len) {
          // Reserves len bytes at pos of the current state (top of stack)
          // or throws an exception
          if (_pos + len <= _size) {
            return;  // All OK, we can just increase tos->pos by len
          }
          JasonCheckSize(_pos + len);

          _buffer.prealloc(len);
          _start = _buffer.data();
          _size = _buffer.size();
        }

        // Here comes infrastructure to sort object index tables:
        static void doActualSortSmall (std::vector<SortEntrySmall>& entries,
                                       uint8_t const* objBase) {
          JASON_ASSERT(entries.size() > 1);
          std::sort(entries.begin(), entries.end(), 
            [objBase] (SortEntrySmall const& a, SortEntrySmall const& b) {
              // return true iff a < b:
              uint8_t const* pa = objBase + a.nameStartOffset;
              uint16_t sizea = a.nameSize;
              uint8_t const* pb = objBase + b.nameStartOffset;
              uint16_t sizeb = b.nameSize;
              size_t const compareLength
                  = static_cast<size_t>((std::min)(sizea, sizeb));
              int res = memcmp(pa, pb, compareLength);

              return (res < 0 || (res == 0 && sizea < sizeb));
            });
        }

        static void doActualSortLarge (std::vector<SortEntryLarge>& entries) {
          JASON_ASSERT(entries.size() > 1);
          std::sort(entries.begin(), entries.end(), 
            [] (SortEntryLarge const& a, SortEntryLarge const& b) {
              // return true iff a < b:
              uint8_t const* pa = a.nameStart;
              uint64_t sizea = a.nameSize;
              uint8_t const* pb = b.nameStart;
              uint64_t sizeb = b.nameSize;
              size_t const compareLength
                  = static_cast<size_t>((std::min)(sizea, sizeb));
              int res = memcmp(pa, pb, compareLength);

              return (res < 0 || (res == 0 && sizea < sizeb));
            });
        };

        static uint8_t const* findAttrName (uint8_t const* base, uint64_t& len) {
          uint8_t const b = *base;
          if (b >= 0x40 && b <= 0xbf) {
            // short UTF-8 string
            len = b - 0x40;
            return base + 1;
          }
          if (b == 0x0c) {
            // long UTF-8 string
            len = 0;
            // read string length
            for (size_t i = 8; i >= 1; i--) {
              len = (len << 8) + base[i];
            }
            return base + 1 + 8; // string starts here
          }
          throw JasonBuilderError("Unimplemented attribute name type.");
        }

        static void sortObjectIndexShort (uint8_t* objBase,
                                          std::vector<JasonLength>& offsets) {
          auto cmp = [&] (JasonLength a, JasonLength b) -> bool {
            uint8_t const* aa = objBase + a;
            uint8_t const* bb = objBase + b;
            if (*aa >= 0x40 && *aa <= 0xbf &&
                *bb >= 0x40 && *bb <= 0xbf) {
              // The fast path, short strings:
              uint8_t m = (std::min)(*aa - 0x40, *bb - 0x40);
              int c = memcmp(aa+1, bb+1, static_cast<size_t>(m));
              return (c < 0 || (c == 0 && *aa < *bb));
            }
            else {
              uint64_t lena;
              uint64_t lenb;
              aa = findAttrName(aa, lena);
              bb = findAttrName(bb, lenb);
              uint64_t m = (std::min)(lena, lenb);
              int c = memcmp(aa, bb, m);
              return (c < 0 || (c == 0 && lena < lenb));
            }
          };
          std::sort(offsets.begin(), offsets.end(), cmp);
        }

        static void sortObjectIndexLong (uint8_t* objBase,
                                         std::vector<JasonLength>& offsets) {
          std::vector<SortEntryLarge>& entries = SortObjectLargeEntries; 
          entries.clear();
          entries.reserve(offsets.size());
          for (JasonLength i = 0; i < offsets.size(); i++) {
            SortEntryLarge e;
            e.offset = offsets[i];
            e.nameStart = findAttrName(objBase + e.offset, e.nameSize);
            entries.push_back(e);
          }
          JASON_ASSERT(entries.size() == offsets.size());
          doActualSortLarge(entries);

          // copy back the sorted offsets 
          for (JasonLength i = 0; i < offsets.size(); i++) {
            offsets[i] = entries[i].offset;
          }
        }

      public:

        JasonOptions options;

        JasonBuilder ()
          : _buffer({ 0 }),
            _pos(0), 
            _attrWritten(false) {
          _start = _buffer.data();
          _size = _buffer.size();
        }

        ~JasonBuilder () {
        }

        JasonBuilder (JasonBuilder const& that) {
          _buffer = that._buffer;
          _start = _buffer.data();
          _size = _buffer.size();
          _pos = that._pos;
          _attrWritten = that._attrWritten;
          _stack = that._stack;
          _index = that._index;
        }

        JasonBuilder& operator= (JasonBuilder const& that) {
          _buffer = that._buffer;
          _start = _buffer.data();
          _size = _buffer.size();
          _pos = that._pos;
          _attrWritten = that._attrWritten;
          _stack = that._stack;
          _index = that._index;
          return *this;
        }

        JasonBuilder (JasonBuilder&& that) {
          _buffer.reset();
          _buffer = that._buffer;
          that._buffer.reset();
          _start = _buffer.data();
          _size = _buffer.size();
          _pos = that._pos;
          _attrWritten = that._attrWritten;
          _stack.clear();
          _stack.swap(that._stack);
          _index.clear();
          _index.swap(that._index);
          that._start = nullptr;
          that._size = 0;
          that._pos = 0;
          that._attrWritten = false;
        }

        JasonBuilder& operator= (JasonBuilder&& that) {
          _buffer.reset();
          _buffer = that._buffer;
          that._buffer.reset();
          _start = _buffer.data();
          _size = _buffer.size();
          _pos = that._pos;
          _attrWritten = that._attrWritten;
          _stack.clear();
          _stack.swap(that._stack);
          _index.clear();
          _index.swap(that._index);
          that._start = nullptr;
          that._size = 0;
          that._pos = 0;
          that._attrWritten = false;
          return *this;
        }

        void clear () {
          _pos = 0;
          _attrWritten = false;
          _stack.clear();
        }

        uint8_t* start () const {
          return _start;
        }

        JasonSlice slice () const {
          return JasonSlice(_start);
        }

        JasonLength size () const {
          // Compute the actual size here, but only when sealed
          if (! _stack.empty()) {
            throw JasonBuilderError("Jason object not sealed.");
          }
          return _pos;
        }

        void add (std::string const& attrName, Jason const& sub) {
          if (_attrWritten) {
            throw JasonBuilderError("Attribute name already written.");
          }
          if (! _stack.empty()) {
            JasonLength& tos = _stack.back();
            if (_start[tos] != 0x07 &&
                _start[tos] != 0x08) {
              throw JasonBuilderError("Need open object for add() call.");
            }
            reportAdd(tos);
          }
          set(Jason(attrName, JasonType::String));
          set(sub);
        }

        uint8_t* add (std::string const& attrName, JasonPair const& sub) {
          if (_attrWritten) {
            throw JasonBuilderError("Attribute name already written.");
          }
          if (! _stack.empty()) {
            JasonLength& tos = _stack.back();
            if (_start[tos] != 0x07 &&
                _start[tos] != 0x08) {
              throw JasonBuilderError("Need open object for add() call.");
            }
            reportAdd(tos);
          }
          set(Jason(attrName, JasonType::String));
          return set(sub);
        }

        void add (Jason const& sub) {
          if (! _stack.empty()) {
            JasonLength& tos = _stack.back();
            if (_start[tos] < 0x05 || _start[tos] > 0x08) {
              // no array or object
              throw JasonBuilderError("Need open array or object for add() call.");
            }
            if (_start[tos] >= 0x07) {   // object or long object
              if (! _attrWritten && ! sub.isString()) {
                throw JasonBuilderError("Need open object for this add() call.");
              }
              if (! _attrWritten) {
                reportAdd(tos);
              }
              _attrWritten = ! _attrWritten;
            }
            else {
              reportAdd(tos);
            }
          }
          set(sub);
        }

        uint8_t* add (JasonPair const& sub) {
          if (! _stack.empty()) {
            JasonLength& tos = _stack.back();
            if (_start[tos] < 0x05 || _start[tos] > 0x08) {
              throw JasonBuilderError("Need open array or object for add() call.");
            }
            if (_start[tos] >= 0x07) {   // object or long object
              if (! _attrWritten && ! sub.isString()) {
                throw JasonBuilderError("Need open object for this add() call.");
              }
              if (! _attrWritten) {
                reportAdd(tos);
              }
              _attrWritten = ! _attrWritten;
            }
            else {
              reportAdd(tos);
            }
          }
          return set(sub);
        }

        void close () {
          if (_stack.empty()) {
            throw JasonBuilderError("Need open array or object for close() call.");
          }
          JasonLength& tos = _stack.back();
          if (_start[tos] < 0x05 || _start[tos] > 0x08) {
            throw JasonBuilderError("Need open array or object for close() call.");
          }
          std::vector<JasonLength>& index = _index[_stack.size() - 1];
          // First determine byte length and its format:
          JasonLength tableBase;
          bool smallByteLength;
          bool smallTable;
          if (index.size() < 0x100 && 
              _pos - tos - 8 + 1 + 2 * index.size() < 0x100) {
            // This is the condition for a one-byte bytelength, since in
            // that case we can save 8 bytes in the beginning and the
            // table fits in the 256 bytes as well, using the small format:
            if (_pos > (tos + 10)) {
              memmove(_start + tos + 2, _start + tos + 10,
                      _pos - (tos + 10));
            }
            _pos -= 8;
            for (size_t i = 0; i < index.size(); i++) {
              index[i] -= 8;
            }
            smallByteLength = true;
            smallTable = true;
          }
          else {
            smallByteLength = false;
            smallTable = index.size() < 0x100 && 
                         (index.size() == 0 || index.back() < 0x10000);
          }
          tableBase = _pos;
          if (smallTable) {
            if (! index.empty()) {
              reserveSpace(2 * index.size() + 1);
              _pos += 2 * index.size() + 1;
            }
            // Make sure we use the small type (6,5 -> 5 and 8,7 -> 7):
            if ((_start[tos] & 1) == 0) {
              --_start[tos];
            }
            if (_start[tos] == 0x07 && 
                index.size() >= 2 &&
                options.sortAttributeNames) {
              sortObjectIndexShort(_start + tos, index);
            }
            for (size_t i = 0; i < index.size(); i++) {
              uint16_t x = static_cast<uint16_t>(index[i]);
              _start[tableBase + 2 * i] = x & 0xff;
              _start[tableBase + 2 * i + 1] = x >> 8;
            }
            _start[_pos - 1] = static_cast<uint8_t>(index.size());
            // Note that for the case of length 0, we store a zero here
            // but further down we overwrite with the bytelength of 2!
          }
          else {
            // large table:
            reserveSpace(8 * index.size() + 8);
            _pos += 8 * index.size() + 8;
            // Make sure we use the large type (6,5 -> 6 and 8.7 -> 8):
            if ((_start[tos] & 1) == 1) {
              ++_start[tos];
            }
            if (_start[tos] == 0x08 && 
                index.size() >= 2 &&
                options.sortAttributeNames) {
              sortObjectIndexLong(_start + tos, index);
            }
            JasonLength x = index.size();
            for (size_t j = 0; j < 8; j++) {
              _start[_pos - 8 + j] = x & 0xff;
              x >>= 8;
            }
            for (size_t i = 0; i < index.size(); i++) {
              x = index[i];
              for (size_t j = 0; j < 8; j++) {
                _start[tableBase + 8 * i + j] = x & 0xff;
                x >>= 8;
              }
            }
          }
          if (smallByteLength) {
            _start[tos + 1] = _pos - tos;
          }
          else {
            _start[tos + 1] = 0x00;
            JasonLength x = _pos - tos;
            for (size_t i = 2; i <= 9; i++) {
              _start[tos + i] = x & 0xff;
              x >>= 8;
            }
          }

          if (options.checkAttributeUniqueness && index.size() > 1 &&
              _start[tos] >= 0x07) {
            // check uniqueness of attribute names
            checkAttributeUniqueness(JasonSlice(_start + tos));
          }

          // Now the array or object is complete, we pop a JasonLength 
          // off the _stack:
          _stack.pop_back();
          // Intentionally leave _index[depth] intact to avoid future allocs!
        }

        JasonBuilder& operator() (std::string const& attrName, Jason sub) {
          add(attrName, sub);
          return *this;
        }

        JasonBuilder& operator() (std::string const& attrName, JasonPair sub) {
          add(attrName, sub);
          return *this;
        }

        JasonBuilder& operator() (Jason sub) {
          add(sub);
          return *this;
        }

        JasonBuilder& operator() (JasonPair sub) {
          add(sub);
          return *this;
        }

        JasonBuilder& operator() () {
          close();
          return *this;
        }

        // returns number of bytes required to store the value
        static JasonLength uintLength (uint64_t value) {
          if (value <= 0xff) {
            // shortcut for the common case
            return 1;
          }
          JasonLength vSize = 0;
          do {
            vSize++;
            value >>= 8;
          } 
          while (value != 0);
          return vSize;
        }

      private:

        void addNull () {
          reserveSpace(1);
          _start[_pos++] = 0x01;
        }

        void addFalse () {
          reserveSpace(1);
          _start[_pos++] = 0x02;
        }

        void addTrue () {
          reserveSpace(1);
          _start[_pos++] = 0x03;
        }

        void addDouble (double v) {
          uint64_t dv;
          memcpy(&dv, &v, sizeof(double));
          JasonLength vSize = sizeof(double);
          reserveSpace(1 + vSize);
          _start[_pos++] = 0x04;
          for (uint64_t x = dv; vSize > 0; vSize--) {
            _start[_pos++] = x & 0xff;
            x >>= 8;
          }
        }

        void addPosInt (uint64_t v) {
          if (v < 8) {
            reserveSpace(1);
            _start[_pos++] = 0x30 + v;
          }
          else if (v > static_cast<uint64_t>(INT64_MAX)) {
            // value is bigger than INT64_MAX. now save as a Double type
            addDouble(static_cast<double>(v));
          }
          else {
            // value is smaller than INT64_MAX: now save as an Int type
            appendUInt(v, 0x17);
          }
        }

        void addNegInt (uint64_t v) {
          if (v < 9) {
            reserveSpace(1);
            if (v == 0) {
              _start[_pos++] = 0x30;
            }
            else {
              _start[_pos++] = 0x40 - v;
            }
          }
          else if (v > static_cast<uint64_t>(- INT64_MIN)) {
            // value is smaller than INT64_MIN. now save as Double
            addDouble(- static_cast<double>(v));
          }
          else {
            // value is bigger than INT64_MIN. now save as Int
            appendUInt(v, 0x1f);
          }
        }

        void addUInt (uint64_t v) {
          if (v < 8) {
            reserveSpace(1);
            _start[_pos++] = 0x30 + v;
          }
          else {
            appendUInt(v, 0x27);
          }
        }

        void addUTCDate (int64_t v) {
          static_assert(sizeof(int64_t) == sizeof(uint64_t), "invalid int64 size");

          uint64_t dv;
          memcpy(&dv, &v, sizeof(int64_t));
          dv = 1 + (~ dv);
          JasonLength vSize = sizeof(int64_t);
          reserveSpace(1 + vSize);
          _start[_pos++] = 0x0d;
          for (uint64_t x = dv; vSize > 0; vSize--) {
            _start[_pos++] = x & 0xff;
            x >>= 8;
          }
        }

        uint8_t* addString (uint64_t strLen) {
          uint8_t* target;
          if (strLen > 127) {
            // long string
            _start[_pos++] = 0x0c;
            // write string length
            appendLength(strLen, 8);
          }
          else {
            // short string
            _start[_pos++] = 0x40 + strLen;
          }
          target = _start + _pos;
          _pos += strLen;
          return target;
        }

        void addCompoundValue (uint8_t type) {
          reserveSpace(10);
          // an array is started:
          _stack.push_back(_pos);
          while (_stack.size() > _index.size()) {
            _index.emplace_back();
          }
          _index[_stack.size() - 1].clear();
          _start[_pos++] = type;
          _start[_pos++] = 0x00;  // Will be filled later with short bytelength
          _pos += 8;              // Possible space for long bytelength
        }

        void addArray () {
          addCompoundValue(0x05);
        }
          
        void addObject () {
          addCompoundValue(0x07);
        }
 
        void set (Jason const& item) {
          auto ctype = item.cType();

          // This method builds a single further Jason item at the current
          // append position. If this is an array or object, then an index
          // table is created and a new JasonLength is pushed onto the stack.
          switch (item.jasonType()) {
            case JasonType::None: {
              throw JasonBuilderError("Cannot set a JasonType::None.");
            }
            case JasonType::Null: {
              reserveSpace(1);
              _start[_pos++] = 0x01;
              break;
            }
            case JasonType::Bool: {
              if (ctype != Jason::CType::Bool) {
                throw JasonBuilderError("Must give bool for JasonType::Bool.");
              }
              reserveSpace(1);
              if (item.getBool()) {
                _start[_pos++] = 0x03;
              }
              else {
                _start[_pos++] = 0x02;
              }
              break;
            }
            case JasonType::Double: {
              double v = 0.0;
              switch (ctype) {
                case Jason::CType::Double:
                  v = item.getDouble();
                  break;
                case Jason::CType::Int64:
                  v = static_cast<double>(item.getInt64());
                  break;
                case Jason::CType::UInt64:
                  v = static_cast<double>(item.getUInt64());
                  break;
                default:
                  throw JasonBuilderError("Must give number for JasonType::Double.");
              }
              reserveSpace(1 + sizeof(double));
              _start[_pos++] = 0x04;
              memcpy(_start + _pos, &v, sizeof(double));
              _pos += sizeof(double);
              break;
            }
            case JasonType::External: {
              if (ctype != Jason::CType::VoidPtr) {
                throw JasonBuilderError("Must give void pointer for JasonType::External.");
              }
              reserveSpace(1 + sizeof(void*));
              // store pointer. this doesn't need to be portable
              _start[_pos++] = 0x09;
              void const* value = item.getExternal();
              memcpy(_start + _pos, &value, sizeof(void*));
              _pos += sizeof(void*);
              break;
            }
            case JasonType::SmallInt: {
              int64_t vv = 0;
              switch (ctype) {
                case Jason::CType::Double:
                  vv = static_cast<int64_t>(item.getDouble());
                  break;
                case Jason::CType::Int64:
                  vv = item.getInt64();
                  break;
                case Jason::CType::UInt64:
                  vv = static_cast<int64_t>(item.getUInt64());
                default:
                  throw JasonBuilderError("Must give number for JasonType::SmallInt.");
              }
              if (vv < -8 || vv > 7) {
                throw JasonBuilderError("Number out of range of JasonType::SmallInt.");
              } 
              reserveSpace(1);
              if (vv >= 0) {
                _start[_pos++] = static_cast<uint8_t>(vv + 0x30);
              }
              else {
                _start[_pos++] = static_cast<uint8_t>(vv + 8 + 0x38);
              }
              break;
            }
            case JasonType::Int: {
              uint64_t v = 0;
              int64_t vv = 0;
              bool positive = true;
              switch (ctype) {
                case Jason::CType::Double:
                  vv = static_cast<int64_t>(item.getDouble());
                  if (vv >= 0) {
                    v = static_cast<uint64_t>(vv);
                    // positive is already set to true
                  }
                  else {
                    v = static_cast<uint64_t>(-vv);
                    positive = false;
                  }
                  break;
                case Jason::CType::Int64:
                  vv = item.getInt64();
                  if (vv >= 0) {
                    v = static_cast<uint64_t>(vv);
                    // positive is already set to true
                  }
                  else {
                    v = static_cast<uint64_t>(-vv);
                    positive = false;
                  }
                  break;
                case Jason::CType::UInt64:
                  v = item.getUInt64();
                  // positive is already set to true
                  break;
                default:
                  throw JasonBuilderError("Must give number for JasonType::Int.");
              }
              JasonLength size = uintLength(v);
              reserveSpace(1 + size);
              if (positive) {
                appendUInt(v, 0x17);
              }
              else {
                appendUInt(v, 0x1f);
              }
              break;
            }
            case JasonType::UInt: {
              uint64_t v = 0;
              switch (ctype) {
                case Jason::CType::Double:
                  if (item.getDouble() < 0.0) {
                    throw JasonBuilderError("Must give non-negative number for JasonType::UInt.");
                  }
                  v = static_cast<uint64_t>(item.getDouble());
                  break;
                case Jason::CType::Int64:
                  if (item.getInt64() < 0) {
                    throw JasonBuilderError("Must give non-negative number for JasonType::UInt.");
                  }
                  v = static_cast<uint64_t>(item.getInt64());
                  break;
                case Jason::CType::UInt64:
                  v = item.getUInt64();
                  break;
                default:
                  throw JasonBuilderError("Must give number for JasonType::UInt.");
              }
              JasonLength size = uintLength(v);
              reserveSpace(1 + size);
              if (item.jasonType() == JasonType::UInt) {
                appendUInt(v, 0x27);
              }
              else {
                appendUInt(v, 0x0f);
              }
              break;
            }
            case JasonType::UTCDate: {
              if (item.jasonType() == JasonType::Int) {
                throw JasonBuilderError("Must give number for JasonType::UTCDate.");
              }
              addUTCDate(item.getInt64());
              break;
            }
            case JasonType::String: {
              if (ctype != Jason::CType::String &&
                  ctype != Jason::CType::CharPtr) {
                throw JasonBuilderError("Must give a string or char const* for JasonType::String.");
              }
              std::string const* s;
              std::string value;
              if (ctype == Jason::CType::String) {
                s = item.getString();
              }
              else {
                value = item.getCharPtr();
                s = &value;
              }
              size_t size = s->size();
              if (size <= 127) {
                // short string
                reserveSpace(1 + size);
                _start[_pos++] = 0x40 + size;
                memcpy(_start + _pos, s->c_str(), size);
                _pos += size;
              }
              else {
                // long string
                reserveSpace(1 + 8 + size);
                _start[_pos++] = 0x0c;
                appendLength(size, 8);
                memcpy(_start + _pos, s->c_str(), size);
              }
              break;
            }
            case JasonType::Array: {
              addArray();
              break;
            }
            case JasonType::Object: {
              addObject();
              break;
            }
            case JasonType::Binary: {
              if (ctype != Jason::CType::String &&
                  ctype != Jason::CType::CharPtr) {
                throw JasonBuilderError("Must give a string or char const* for JasonType::Binary.");
              }
              std::string const* s;
              std::string value;
              if (ctype == Jason::CType::String) {
                s = item.getString();
              }
              else {
                value = item.getCharPtr();
                s = &value;
              }
              JasonLength v = s->size();
              JasonLength size = uintLength(v);
              reserveSpace(1 + size + v);
              appendUInt(v, 0xbf);
              memcpy(_start + _pos, s->c_str(), v);
              _pos += v;
              break;
            }
            case JasonType::ArangoDB_id: {
              reserveSpace(1);
              _start[_pos++] = 0x0b;
              break;
            }
            case JasonType::ID: {
              throw JasonBuilderError("Need a JasonPair to build a JasonType::ID.");
            }
            case JasonType::BCD: {
              throw JasonBuilderError("BCD not yet implemented.");
            }
          }
        }

        uint8_t* set (JasonPair const& pair) {
          // This method builds a single further Jason item at the current
          // append position. This is the case for JasonType::ID or
          // JasonType::Binary, which both need two pieces of information
          // to build.
          if (pair.jasonType() == JasonType::ID) {
            reserveSpace(1);
            _start[_pos++] = 0x0a;
            set(Jason(pair.getSize(), JasonType::UInt));
            set(Jason(reinterpret_cast<char const*>(pair.getStart()),
                      JasonType::String));
            return nullptr;  // unused here
          }
          else if (pair.jasonType() == JasonType::Binary) {
            uint64_t v = pair.getSize();
            JasonLength size = uintLength(v);
            reserveSpace(1 + size + v);
            appendUInt(v, 0xbf);
            memcpy(_start + _pos, pair.getStart(), v);
            _pos += v;
            return nullptr;  // unused here
          }
          else if (pair.jasonType() == JasonType::String) {
            uint64_t size = pair.getSize();
            if (size > 127) { 
              // long string
              reserveSpace(1 + 8 + size);
              _start[_pos++] = 0x0c;
              appendLength(size, 8);
              _pos += size;
            }
            else {
              // short string
              reserveSpace(1 + size);
              _start[_pos++] = 0x40 + size;
              _pos += size;
            }
            // Note that the data is not filled in! It is the responsibility
            // of the caller to fill in 
            //   _start + _pos - size .. _start + _pos - 1
            // with valid UTF-8!
            return _start + _pos - size;
          }
          throw JasonBuilderError("Only JasonType::ID, JasonType::Binary and JasonType::String are valid for JasonPair argument.");
        }

        void reportAdd (JasonLength base) {
          size_t depth = _stack.size() - 1;
          _index[depth].push_back(_pos - base);
        }

        void appendLength (JasonLength v, uint64_t n) {
          reserveSpace(n);
          for (uint64_t i = 0; i < n; ++i) {
            _start[_pos++] = v & 0xff;
            v >>= 8;
          }
        }

        void appendUInt (uint64_t v, uint8_t base) {
          JasonLength vSize = uintLength(v);
          reserveSpace(1 + vSize);
          _start[_pos++] = base + vSize;
          for (uint64_t x = v; vSize > 0; vSize--) {
            _start[_pos++] = x & 0xff;
            x >>= 8;
          }
        }

        void appendInt (int64_t v) {
          if (v >= 0) {
            appendUInt(static_cast<uint64_t>(v), 0x17);
          }
          else {
            appendUInt(static_cast<uint64_t>(-v), 0x1f);
          }
        }
 
        void checkAttributeUniqueness (JasonSlice const obj) const {
          JasonLength const n = obj.length();
          JasonSlice previous = obj.keyAt(0);
          JasonLength len;
          char const* p = previous.getString(len);

          for (JasonLength i = 1; i < n; ++i) {
            JasonSlice current = obj.keyAt(i);
            if (! current.isString()) {
              return;
            }
            
            JasonLength len2;
            char const* q = current.getString(len2);

            if (len == len2 && memcmp(p, q, len2) == 0) {
              // identical key
              throw JasonBuilderError("duplicate attribute name.");
            }
            // re-use already calculated values for next round
            len = len2;
            p = q;

            // recurse into sub-objects
            JasonSlice value = obj.valueAt(i);
            if (value.isObject()) {
              checkAttributeUniqueness(value);
            }
          } 
        }
    };

  }  // namespace arangodb::jason
}  // namespace arangodb

#endif
