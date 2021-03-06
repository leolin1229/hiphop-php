/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   | Copyright (c) 1997-2010 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include <runtime/base/util/request_local.h>
#include <runtime/ext/thrift/transport.h>
#include <runtime/ext/ext_reflection.h>
#include <runtime/ext/ext_thrift.h>

#include <stack>
#include <utility>

namespace HPHP {

static const uint8_t VERSION_MASK = 0x1f;
static const uint8_t VERSION = 2;
static const uint8_t VERSION_LOW = 1;
static const uint8_t VERSION_DOUBLE_BE = 2;
static const uint8_t PROTOCOL_ID = 0x82;
static const uint8_t TYPE_MASK = 0xe0;
static const uint8_t TYPE_SHIFT_AMOUNT = 5;

enum CState {
  STATE_CLEAR,
  STATE_FIELD_WRITE,
  STATE_VALUE_WRITE,
  STATE_CONTAINER_WRITE,
  STATE_BOOL_WRITE,
  STATE_FIELD_READ,
  STATE_CONTAINER_READ,
  STATE_VALUE_READ,
  STATE_BOOL_READ
};

enum CType {
  C_STOP = 0x00,
  C_TRUE = 0x01,
  C_FALSE = 0x02,
  C_BYTE = 0x03,
  C_I16 = 0x04,
  C_I32 = 0x05,
  C_I64 = 0x06,
  C_DOUBLE = 0x07,
  C_BINARY = 0x08,
  C_LIST = 0x09,
  C_SET = 0x0A,
  C_MAP = 0x0B,
  C_STRUCT = 0x0C
};

enum CListType {
  C_LIST_LIST,
  C_LIST_SET
};

enum TResponseType {
  T_REPLY = 2,
  T_EXCEPTION = 3
};

static CType ttype_to_ctype(TType x) {
  switch (x) {
    case T_STOP:
      return C_STOP;
    case T_BOOL:
      return C_TRUE;
    case T_BYTE:
      return C_BYTE;
    case T_I16:
      return C_I16;
    case T_I32:
      return C_I32;
    case T_I64:
      return C_I64;
    case T_DOUBLE:
      return C_DOUBLE;
    case T_STRING:
      return C_BINARY;
    case T_STRUCT:
      return C_STRUCT;
    case T_LIST:
      return C_LIST;
    case T_SET:
      return C_SET;
    case T_MAP:
      return C_MAP;
    default:
      throw InvalidArgumentException("unknown TType", x);
  }
}

static TType ctype_to_ttype(CType x) {
  switch (x) {
    case C_STOP:
      return T_STOP;
    case C_TRUE:
    case C_FALSE:
      return T_BOOL;
    case C_BYTE:
      return T_BYTE;
    case C_I16:
      return T_I16;
    case C_I32:
      return T_I32;
    case C_I64:
      return T_I64;
    case C_DOUBLE:
      return T_DOUBLE;
    case C_BINARY:
      return T_STRING;
    case C_STRUCT:
      return T_STRUCT;
    case C_LIST:
        return T_LIST;
    case C_SET:
        return T_SET;
    case C_MAP:
        return T_MAP;
    default:
        throw InvalidArgumentException("unknown CType", x);
  }
}

enum TError {
  ERR_UNKNOWN = 0,
  ERR_INVALID_DATA = 1,
  ERR_BAD_VERSION = 4
};

class CompactRequestData : public RequestEventHandler {
  public:
    CompactRequestData() : version(VERSION) { }
    void clear() { version = VERSION; }

    virtual void requestInit() {
      clear();
    }

    virtual void requestShutdown() {
      clear();
    }

    uint8_t version;
};
IMPLEMENT_STATIC_REQUEST_LOCAL(CompactRequestData, s_compact_request_data);

static void thrift_error(CStrRef what, TError why) ATTRIBUTE_NORETURN;
static void thrift_error(CStrRef what, TError why) {
  throw create_object("TProtocolException", CREATE_VECTOR2(what, why));
}

class CompactWriter {
  public:
    CompactWriter(CObjRef _transportobj) :
      transport(_transportobj),
      version(VERSION),
      state(STATE_CLEAR),
      lastFieldNum(0),
      boolFieldNum(0),
      structHistory(),
      containerHistory() {
    }

    void setWriteVersion(uint8_t _version) {
      version = _version;
    }

    void writeHeader(CStrRef name, uint8_t msgtype, uint32_t seqid) {
      writeUByte(PROTOCOL_ID);
      writeUByte(version | (msgtype << TYPE_SHIFT_AMOUNT));
      writeVarint(seqid);
      writeString(name);

      state = STATE_VALUE_WRITE;
    }

    void write(CObjRef obj) {
      writeStruct(obj);
    }

  private:
    PHPOutputTransport transport;
    uint8_t version;
    CState state;
    uint16_t lastFieldNum;
    uint16_t boolFieldNum;
    std::stack<std::pair<CState, uint16_t> > structHistory;
    std::stack<CState> containerHistory;

    void writeStruct(CObjRef obj) {
      // Save state
      structHistory.push(std::make_pair(state, lastFieldNum));
      state = STATE_FIELD_WRITE;
      lastFieldNum = 0;

      // Get field specification
      CArrRef spec = f_hphp_get_static_property(obj->o_getClassName(), "_TSPEC")
        .toArray();

      // Write each member
      for (ArrayIter specIter = spec.begin(); !specIter.end(); ++specIter) {
        Variant key = specIter.first();
        if (!key.isInteger()) {
          thrift_error("Bad keytype in TSPEC (expected 'long')",
            ERR_INVALID_DATA);
        }

        uint16_t fieldNo = key.toInt16();
        Array fieldSpec = specIter.second().toArray();

        String fieldName = fieldSpec
          .rvalAt(s_var, AccessFlags::Error_Key).toString();
        TType fieldType = (TType)fieldSpec
          .rvalAt(s_type, AccessFlags::Error_Key).toByte();

        Variant fieldVal = obj->o_get(fieldName, true, obj->o_getClassName());

        if (!fieldVal.isNull()) {
          writeFieldBegin(fieldNo, fieldType);
          writeField(fieldVal, fieldSpec, fieldType);
          writeFieldEnd();
        }
      }

      // Write stop
      writeUByte(0);

      // Restore state
      std::pair<CState, uint16_t> prev = structHistory.top();
      state = prev.first;
      lastFieldNum = prev.second;
      structHistory.pop();
    }

    void writeFieldBegin(uint16_t fieldNum, TType fieldType) {
      if (fieldType == T_BOOL) {
        state = STATE_BOOL_WRITE;
        boolFieldNum = fieldNum;
        // the value and header are written together in writeField
      } else {
        state = STATE_VALUE_WRITE;
        writeFieldHeader(fieldNum, ttype_to_ctype(fieldType));
      }
    }

    void writeFieldEnd(void) {
      state = STATE_FIELD_WRITE;
    }

    void writeFieldHeader(uint16_t fieldNum, CType fieldType) {
      int delta = fieldNum - lastFieldNum;

      if (0 < delta && delta <= 15) {
        writeUByte((delta << 4) | fieldType);
      } else {
        writeUByte(fieldType);
        writeI(fieldNum);
      }

      lastFieldNum = fieldNum;
    }

    void writeField(CVarRef value,
                    CArrRef valueSpec,
                    TType type) {
      switch (type) {
        case T_STOP:
        case T_VOID:
          break;

        case T_STRUCT:
          if (!value.is(KindOfObject)) {
            thrift_error("Attempt to send non-object type as T_STRUCT",
              ERR_INVALID_DATA);
          }
          writeStruct(value);
          break;

        case T_BOOL: {
            bool b = value.toBoolean();

            if (state == STATE_BOOL_WRITE) {
              CType t = b ? C_TRUE : C_FALSE;
              writeFieldHeader(boolFieldNum, t);
            } else if (state == STATE_CONTAINER_WRITE) {
              writeUByte(b ? C_TRUE : C_FALSE);
            } else {
              thrift_error("Invalid state in compact protocol", ERR_UNKNOWN);
            }
          }
          break;

        case T_BYTE:
          writeUByte(value.toByte());
          break;

        case T_I16:
        case T_I32:
        case T_I64:
        case T_U64:
          writeI(value.toInt64());
          break;

        case T_DOUBLE: {
            union {
              uint64_t i;
              double d;
            } u;

            u.d = value.toDouble();
            uint64_t bits;
            if (version >= VERSION_DOUBLE_BE) {
              bits = htonll(u.i);
            } else {
              bits = htolell(u.i);
            }

            transport.write((char*)&bits, 8);
          }
          break;

        case T_UTF8:
        case T_UTF16:
        case T_STRING: {
            String s = value.toString();
            uint32_t len = s.size();
            writeVarint(len);
            transport.write(s, len);
            break;
          }

        case T_MAP:
          writeMap(value.toArray(), valueSpec);
          break;

        case T_LIST:
          writeList(value.toArray(), valueSpec, C_LIST_LIST);
          break;

        case T_SET:
          writeList(value.toArray(), valueSpec, C_LIST_SET);
          break;

        default:
          throw InvalidArgumentException("unknown TType", type);
      }
    }

    void writeMap(Array arr, CArrRef spec) {
      TType keyType = (TType)spec
        .rvalAt(s_ktype, AccessFlags::Error_Key).toByte();
      TType valueType = (TType)spec
        .rvalAt(s_vtype, AccessFlags::Error_Key).toByte();

      Array keySpec = spec.rvalAt(s_key, AccessFlags::Error_Key).toArray();
      Array valueSpec = spec.rvalAt(s_val, AccessFlags::Error_Key).toArray();

      writeMapBegin(keyType, valueType, arr.size());

      for (ArrayIter arrIter = arr.begin(); !arrIter.end(); ++arrIter) {
        writeField(arrIter.first(), keySpec, keyType);
        writeField(arrIter.second(), valueSpec, valueType);
      }

      writeCollectionEnd();
    }

    void writeList(Array arr, CArrRef spec, CListType listType) {
      TType valueType = (TType)spec
        .rvalAt(s_etype, AccessFlags::Error_Key).toByte();
      Array valueSpec = spec
        .rvalAt(s_elem, AccessFlags::Error_Key).toArray();

      writeListBegin(valueType, arr.size());

      for (ArrayIter arrIter = arr.begin(); !arrIter.end(); ++arrIter) {
        Variant x;
        if (listType == C_LIST_LIST) {
          x = arrIter.second();
        } else if (listType == C_LIST_SET) {
          x = arrIter.first();
        }

        writeField(x, valueSpec, valueType);
      }

      writeCollectionEnd();
    }

    void writeMapBegin(TType keyType, TType valueType,  uint32_t size) {
      if (size == 0) {
        writeUByte(0);
      } else {
        writeVarint(size);

        CType keyCType = ttype_to_ctype(keyType);
        CType valueCType = ttype_to_ctype(valueType);
        writeUByte((keyCType << 4) | valueCType);
      }

      containerHistory.push(state);
      state = STATE_CONTAINER_WRITE;
    }

    void writeListBegin(TType elemType, uint32_t size) {
      if (size <= 14) {
        writeUByte((size << 4) | ttype_to_ctype(elemType));
      } else {
        writeUByte(0xf0 | ttype_to_ctype(elemType));
        writeVarint(size);
      }

      containerHistory.push(state);
      state = STATE_CONTAINER_WRITE;
    }

    void writeCollectionEnd(void) {
      state = containerHistory.top();
      containerHistory.pop();
    }

    void writeUByte(uint8_t n) {
      transport.writeI8(n);
    }

    void writeI(int64_t n) {
      writeVarint(i64ToZigzag(n));
    }

    void writeVarint(uint64_t n) {
      uint8_t buf[10];
      uint8_t wsize = 0;

      while (true) {
        if ((n & ~0x7FL) == 0) {
          buf[wsize++] = (int8_t)n;
          break;
        } else {
          buf[wsize++] = (int8_t)((n & 0x7F) | 0x80);
          n >>= 7;
        }
      }

      transport.write((char*)buf, wsize);
    }

    void writeString(CStrRef s) {
      uint32_t len = s.size();
      writeVarint(len);
      transport.write(s, len);
    }

    uint64_t i64ToZigzag(int64_t n) {
      return (n << 1) ^ (n >> 63);
    }
};

class CompactReader {
  public:
    CompactReader(CObjRef _transportobj) :
      transport(_transportobj),
      version(VERSION),
      state(STATE_CLEAR),
      lastFieldNum(0),
      boolValue(true),
      structHistory(),
      containerHistory() {
    }

    Variant read(CStrRef resultClassName) {
      uint8_t protoId = readUByte();
      if (protoId != PROTOCOL_ID) {
        thrift_error("Bad protocol id in TCompact message", ERR_BAD_VERSION);
      }

      uint8_t versionAndType = readUByte();
      uint8_t type = (versionAndType & TYPE_MASK) >> TYPE_SHIFT_AMOUNT;
      version = versionAndType & VERSION_MASK;
      if (version < VERSION_LOW || version > VERSION) {
        thrift_error("Bad version in TCompact message", ERR_BAD_VERSION);
      }

      // TODO: we eventually want to return seqid to the caller
      /*uint32_t seqid =*/ readVarint(); // unused
      /*Variant name =*/ readString(); // unused

      if (type == T_REPLY) {
        Object ret = create_object(resultClassName, Array());
        Variant spec = f_hphp_get_static_property(resultClassName, "_TSPEC");
        readStruct(ret, spec);
        return ret;
      } else if (type == T_EXCEPTION) {
        Object exn = create_object("TApplicationException", Array());
        Variant spec = f_hphp_get_static_property("TApplicationException", "_TSPEC");
        readStruct(exn, spec);
        throw exn;
      } else {
        thrift_error("Invalid response type", ERR_INVALID_DATA);
      }
    }

  private:
    PHPInputTransport transport;
    uint8_t version;
    CState state;
    uint16_t lastFieldNum;
    bool boolValue;
    std::stack<std::pair<CState, uint16_t> > structHistory;
    std::stack<CState> containerHistory;

    void readStruct(CObjRef dest, CArrRef spec) {
      readStructBegin();

      while (true) {
        int16_t fieldNum;
        TType fieldType;
        readFieldBegin(fieldNum, fieldType);

        if (fieldType == T_STOP) {
          break;
        }

        bool readComplete = false;

        Variant fieldSpecVariant = spec.rvalAt(fieldNum);
        if (!fieldSpecVariant.isNull()) {
          Array fieldSpec = fieldSpecVariant.toArray();

          String fieldName = fieldSpec.rvalAt(s_var).toString();
          TType expectedType = (TType)fieldSpec.rvalAt(s_type).toInt64();

          if (typesAreCompatible(fieldType, expectedType)) {
            readComplete = true;
            Variant fieldValue = readField(fieldSpec, fieldType);
            dest->o_set(fieldName, fieldValue, dest->o_getClassName());
          }
        }

        if (!readComplete) {
          skip(fieldType);
        }

        readFieldEnd();
      }

      readStructEnd();
    }

    void readStructBegin(void) {
      structHistory.push(std::make_pair(state, lastFieldNum));
      state = STATE_FIELD_READ;
      lastFieldNum = 0;
    }

    void readStructEnd(void) {
      std::pair<CState, uint16_t> prev = structHistory.top();
      state = prev.first;
      lastFieldNum = prev.second;
      structHistory.pop();
    }

    void readFieldBegin(int16_t &fieldNum, TType &fieldType) {
      uint8_t fieldTypeAndDelta = readUByte();
      int delta = fieldTypeAndDelta >> 4;
      CType fieldCType = (CType)(fieldTypeAndDelta & 0x0f);
      fieldType = ctype_to_ttype(fieldCType);

      if (fieldCType == C_STOP) {
        fieldNum = 0;
        return;
      }

      if (delta == 0) {
        fieldNum = readI();
      } else {
        fieldNum = lastFieldNum + delta;
      }

      lastFieldNum = fieldNum;

      if (fieldCType == C_TRUE) {
        state = STATE_BOOL_READ;
        boolValue = true;
      } else if (fieldCType == C_FALSE) {
        state = STATE_BOOL_READ;
        boolValue = false;
      } else {
        state = STATE_VALUE_READ;
      }
    }

    void readFieldEnd(void) {
      state = STATE_FIELD_READ;
    }

    Variant readField(CArrRef spec, TType type) {
      switch (type) {
        case T_STOP:
        case T_VOID:
          return uninit_null();

        case T_STRUCT: {
            Variant className = spec.rvalAt(s_class);
            if (className.isNull()) {
              thrift_error("no class type in spec", ERR_INVALID_DATA);
            }

            String classNameString = className.toString();
            Variant newStruct = create_object(classNameString, Array());
            if (newStruct.isNull()) {
              thrift_error("invalid class type in spec", ERR_INVALID_DATA);
            }

            Variant newStructSpec =
              f_hphp_get_static_property(classNameString, "_TSPEC");

            if (!newStructSpec.is(KindOfArray)) {
              thrift_error("invalid type of spec", ERR_INVALID_DATA);
            }

            readStruct(newStruct, newStructSpec);
            return newStruct;
          }

        case T_BOOL:
          if (state == STATE_BOOL_READ) {
            return boolValue;
          } else if (state == STATE_CONTAINER_READ) {
            return (readUByte() == C_TRUE);
          } else {
            thrift_error("Invalid state in compact protocol", ERR_UNKNOWN);
          }

        case T_BYTE:
          return readUByte();

        case T_I16:
        case T_I32:
        case T_I64:
        case T_U64:
          return readI();

        case T_DOUBLE: {
            union {
              uint64_t i;
              double d;
            } u;

            transport.readBytes(&(u.i), 8);
            if (version >= VERSION_DOUBLE_BE) {
              u.i = ntohll(u.i);
            } else {
              u.i = letohll(u.i);
            }
            return u.d;
          }

        case T_UTF8:
        case T_UTF16:
        case T_STRING:
          return readString();

        case T_MAP:
          return readMap(spec);

        case T_LIST:
          return readList(spec, C_LIST_LIST);

        case T_SET:
          return readList(spec, C_LIST_SET);

        default:
          throw InvalidArgumentException("unknown TType", type);
      }
    }

    void skip(TType type) {
      switch (type) {
        case T_STOP:
        case T_VOID:
          break;

        case T_STRUCT: {
            readStructBegin();

            while (true) {
              int16_t fieldNum;
              TType fieldType;
              readFieldBegin(fieldNum, fieldType);

              if (fieldType == T_STOP) {
                break;
              }

              skip(fieldType);

              readFieldEnd();
            }

            readStructEnd();
          }
          break;

        case T_BOOL:
          if (state == STATE_BOOL_READ) {
            // don't need to do anything
          } else if (state == STATE_CONTAINER_READ) {
            readUByte();
          } else {
            thrift_error("Invalid state in compact protocol", ERR_UNKNOWN);
          }
          break;

        case T_BYTE:
          readUByte();
          break;

        case T_I16:
        case T_I32:
        case T_I64:
        case T_U64:
          readI();
          break;

        case T_DOUBLE:
          transport.skip(8);
          break;

        case T_UTF8:
        case T_UTF16:
        case T_STRING:
          transport.skip(readVarint());
          break;

        case T_MAP: {
            TType keyType, valueType;
            uint32_t size;
            readMapBegin(keyType, valueType, size);

            for (uint32_t i = 0; i < size; i++) {
              skip(keyType);
              skip(valueType);
            }

            readCollectionEnd();
          }
          break;

        case T_LIST:
        case T_SET: {
            TType valueType;
            uint32_t size;
            readListBegin(valueType, size);

            for (uint32_t i = 0; i < size; i++) {
              skip(valueType);
            }

            readCollectionEnd();
          }
          break;

        default:
          throw InvalidArgumentException("unknown TType", type);
      }
    }

    Variant readMap(CArrRef spec) {
      TType keyType, valueType;
      uint32_t size;
      readMapBegin(keyType, valueType, size);

      Array keySpec = spec.rvalAt(s_key, AccessFlags::Error);
      Array valueSpec = spec.rvalAt(s_val, AccessFlags::Error);
      Variant ret = Array::Create();

      for (uint32_t i = 0; i < size; i++) {
        Variant key = readField(keySpec, keyType);
        Variant value = readField(valueSpec, valueType);
        ret.set(key, value);
      }

      readCollectionEnd();
      return ret;
    }

    Variant readList(CArrRef spec, CListType listType) {
      TType valueType;
      uint32_t size;
      readListBegin(valueType, size);

      Array valueSpec = spec.rvalAt(s_elem, AccessFlags::Error_Key);
      Variant ret = Array::Create();

      for (uint32_t i = 0; i < size; i++) {
        Variant value = readField(valueSpec, valueType);
        if (listType == C_LIST_LIST) {
          ret.append(value);
        } else {
          ret.set(value, true);
        }
      }

      readCollectionEnd();
      return ret;
    }

    void readMapBegin(TType &keyType, TType &valueType, uint32_t &size) {
      size = readVarint();
      uint8_t types = 0;
      if (size > 0) {
        types = readUByte();
      }

      valueType = ctype_to_ttype((CType)(types & 0x0f));
      keyType = ctype_to_ttype((CType)(types >> 4));

      containerHistory.push(state);
      state = STATE_CONTAINER_READ;
    }

    void readListBegin(TType &elemType, uint32_t &size) {
      uint8_t sizeAndType = readUByte();

      size = sizeAndType >> 4;
      elemType = ctype_to_ttype((CType)(sizeAndType & 0x0f));

      if (size == 15) {
        size = readVarint();
      }

      containerHistory.push(state);
      state = STATE_CONTAINER_READ;
    }

    void readCollectionEnd(void) {
      state = containerHistory.top();
      containerHistory.pop();
    }

    uint8_t readUByte(void) {
      return transport.readI8();
    }

    int64_t readI(void) {
      return zigzagToI64(readVarint());
    }

    uint64_t readVarint(void) {
      uint64_t result = 0;
      uint8_t shift = 0;

      while (true) {
        uint8_t byte = readUByte();
        result |= (uint64_t)(byte & 0x7f) << shift;
        shift += 7;

        if (!(byte & 0x80)) {
          return result;
        }

        // Should never read more than 10 bytes, which is the max for a 64-bit
        // int
        if (shift >= 10 * 7) {
          thrift_error("Variable-length int over 10 bytes", ERR_INVALID_DATA);
        }
      }
    }

    Variant readString(void) {
      uint32_t size = readVarint();

      if (size && (size + 1)) {
        String s = String(size, ReserveString);
        char* buf = s.mutableSlice().ptr;

        transport.readBytes(buf, size);
        return s.setSize(size);
      } else {
        transport.skip(size);
        return "";
      }
    }

    int64_t zigzagToI64(uint64_t n) {
      return (n >> 1) ^ -(n & 1);
    }

    bool typeIsInt(TType t) {
      return (t == T_BYTE) || ((t >= T_I16) && (t <= T_I64));
    }

    bool typesAreCompatible(TType t1, TType t2) {
      return (t1 == t2) || (typeIsInt(t1) && (typeIsInt(t2)));
    }

};

int f_thrift_protocol_set_compact_version(int version) {
  int result = s_compact_request_data->version;
  s_compact_request_data->version = (uint8_t)version;
  return result;
}

void f_thrift_protocol_write_compact(CObjRef transportobj,
                                     CStrRef method_name,
                                     int64_t msgtype,
                                     CObjRef request_struct,
                                     int seqid) {
  CompactWriter writer(transportobj);
  writer.setWriteVersion(s_compact_request_data->version);
  writer.writeHeader(method_name, (uint8_t)msgtype, (uint32_t)seqid);
  writer.write(request_struct);
}

Variant f_thrift_protocol_read_compact(CObjRef transportobj,
                                       CStrRef obj_typename) {
  CompactReader reader(transportobj);
  return reader.read(obj_typename);
}

}
