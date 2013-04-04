#!/usr/bin/env python

import yaml
import sys
import os

# This will be used to collect all the enums defined so they can be handled later on
types_enum = {
}
types_struct = {
}
def type_get(name):
    types = (types_base, types_enum, types_struct)
    for type_dict in types:
        t = type_dict.get(name, None)
        if t is not None:
            return t
    assert False, "Unknown type name '%s'" % name

class BaseType:
    def __init__(self, name, params):
        self.name = name

    def define(self):
        return '%s %s' % (self.type_name, self.name)

    def marshall_type(self):
        return '%d'

    def marshall_func(self):
        return ''

    def emit_marshall_call(self, field_name):
        field_values = dict(field_name=field_name, field_marshall_type=self.marshall_type(), field_marshall_func=self.marshall_func())
        print '\tfprintf(f, "(%(field_name)s %(field_marshall_type)s)", %(field_marshall_func)s(%(field_name)s));' % field_values

class TypeBool(BaseType):
    type_name = 'bool'

class TypeInt(BaseType):
    type_name = 'int'

class TypeUInt16(BaseType):
    type_name = 'uint16_t'
    def marshall_type(self):
        return '%u'

class TypeDouble(BaseType):
    type_name = 'double'
    def marshall_type(self):
        return '%g'

class TypeArray(BaseType):
    def __init__(self, name, params):
        self.name = name
        self.array_len = from_map(params, 'len')
        self.array_type_defn = from_map(params, 'array_type')
        self.array_type = type_definition('array_field_name', self.array_type_defn)

    def array_type_name(self):
        return self.array_type.type_name

    def define(self):
        return '%s %s[%s]' % (self.array_type_name(), self.name, self.array_len)

    def marshall_type(self):
        return 'NA'

    def marshall_func(self):
        return 'NA'

    def emit_marshall_call(self, field_name):
        print '\tfprintf(f, "(%s ");' % self.name
        print '\t{'
        print '\t\tint i;'
        print '\t\tfor (i = 0; i < %s; i++) {' % self.array_len
        print '\t\t\t',
        field_name = '%s[i]' % field_name
        self.array_type.emit_marshall_call(field_name)
        print '\t\t}'
        print '\t}'
        print '\tfprintf(f, ")");'

class TypeString(BaseType):
    def __init__(self, name, params):
        self.name = name
        self.str_len = from_map(params, 'len')

    def define(self):
        return 'char %s[%s]' % (self.name, self.str_len)

    def marshall_type(self):
        return '\\"%s\\"'

types_base = {
    'bool': TypeBool,
    'int': TypeInt,
    'uint16_t': TypeUInt16,
    'double': TypeDouble,
    'array': TypeArray,
    'string': TypeString,
}

def from_map(mapping, key_name):
    for key, val in mapping:
        if key.value == key_name:
            return val.value
    raise ValueError('No mapping for %s in map %s' % (key_name, mapping))

def from_map_default(mapping, key_name, default):
    try:
        return from_map(mapping, key_name)
    except ValueError:
        return default

def type_decode(field_def):
    print field_def
    type_name = field_def
    type_param = None
    if type(field_def) == dict:
        assert len(field_def.keys()) == 1
        type_name = field_def.keys()[0]
        type_param = field_def[type_name]
    if type(field_def) == list:
        type_name = field_def[0]
        type_param = field_def[1]
    return (type_name, type_param)

def type_definition(field_name, field_def):
    type_name = from_map(field_def, 'type')
    t = type_get(type_name)
    assert t is not None, "Type '%s' is unknown, field_def=%s" % (type_name, field_def)
    try:
        name = field_name.value
    except AttributeError:
        name = field_name
    return t(name, field_def)

class KindStruct:
    def __init__(self, base_name, defn):
        self.base_name = base_name
        self.name = '%s_t' % base_name
        self.fields = []
        for field_name, field_def in defn:
            t = type_definition(field_name, field_def.value)
            self.fields.append(t)
        types_enum[self.base_name] = self.make_type()

    def emit_definition(self):
        print 'typedef struct %s {' % self.name
        for field in self.fields:
            print '\t%s;' % field.define()
        print '} %s;' % self.name

    def marshall_func_name(self):
        return 'marshall_struct_%s' % self.base_name

    def marshall_declaration(self, marshall):
        if marshall:
            prefix = 'marshall'
        else:
            prefix = 'unmarshall'
        values = dict(struct_name=self.name, base_name=self.base_name, prefix=prefix, func_name = self.marshall_func_name())

        return 'static inline void %(func_name)s(%(struct_name)s *data, FILE *f)' % values

    def emit_marshall_declaration(self):
        print '%s;' % self.marshall_declaration(True)
        #print '%s;' % self.marshall_declaration(False)

    def emit_marshall_definition(self):
        print '%s' % self.marshall_declaration(True)
        print '{'
        for field in self.fields:
            field_name = 'data->%s' % field.name
            field.emit_marshall_call(field_name)
        print '}'
        #print '%s {}' % self.marshall_declaration(False)

    def make_type(self):
        class TypeStructGenerated:
            base_name = self.base_name
            type_name = self.name
            marshall_func_name = self.marshall_func_name()
            def __init__(self, name, params):
                given_type = from_map(params, 'type')
                assert given_type == self.base_name, 'got type "%s" but expected "%s"' % (given_type, self.base_name)
                self.name = name
            def define(self):
                return '%s %s' % (self.type_name, self.name)
            def marshall_type(self):
                return '\\"%s\\"'
            def marshall_func(self):
                return self.marshall_func_name
            def emit_marshall_call(self, field_name):
                print '\tfprintf(f, "(%s ");' % self.name
                print '\t%s(&%s, f);' % (self.marshall_func_name, field_name)
                print '\tfprintf(f, ")");'

        TypeStructGenerated.__name__ = 'Type%s' % str(self.base_name.capitalize())
        return TypeStructGenerated


class KindConst:
    def __init__(self, name, defn):
        self.name = name
        self.defn = type_definition(name, defn)
        self.value = from_map(defn, 'value')

    def emit_definition(self):
        print 'const %s = %s;' % (self.defn.define(), self.value)

class KindDefine:
    def __init__(self, name, defn):
        self.name = name
        self.value = from_map(defn, 'value')

    def emit_definition(self):
        print '#define %s %s' % (self.name, self.value)

    def emit_marshall_declaration(self):
        pass
    def emit_marshall_definition(self):
        pass

class KindEnum:
    def __init__(self, base_name, defn):
        self.base_name = base_name
        self.name = '%s_e' % base_name
        types_enum[self.base_name] = self.make_type()
        # An enum can come with no prefix
        self.prefix = from_map_default(defn, 'prefix', '')
        values = []
        for key, val in from_map(defn, 'values'):
            values.append(key.value)
        self.values  = values

    def emit_definition(self):
        print 'typedef enum %s {' % self.name
        s = 'enum %s {\n' % self.name
        for value in self.values:
            print '\t%s%s,\n' % (self.prefix, value)
        print '} %s;' % self.name

    def marshall_func_name(self):
        return 'marshall_enum_%s' % self.base_name

    def marshall_definition(self):
        return 'static inline const char *%s(%s val)' % (self.marshall_func_name(), self.name)

    def emit_marshall_definition(self):
        print self.marshall_definition()
        print '{'
        print '\tswitch (val) {'
        for val in self.values:
            print '\t\tcase %s: return "%s";' % (self.prefix+val, val)
        print '\t}'
        print '\treturn %s;' % (self.prefix+'UNKNOWN')
        print '}'

    def emit_marshall_declaration(self):
        print '%s;' % self.marshall_definition()

    def make_type(self):
        class TypeEnumGenerated(BaseType):
            base_name = self.base_name
            type_name = self.name
            marshall_func_name = self.marshall_func_name()
            def __init__(self, name, params):
                assert from_map(params, 'type') == self.base_name
                self.name = name
            def define(self):
                return '%s %s' % (self.type_name, self.name)
            def marshall_type(self):
                return '\\"%s\\"'
            def marshall_func(self):
                return self.marshall_func_name

        TypeEnumGenerated.__name__ = 'Type%s' % str(self.base_name.capitalize())
        return TypeEnumGenerated


class KindInclude:
    TYPE_GLOBAL = 'global'
    TYPE_LOCAL = 'local'

    def __init__(self, name, defn):
        assert name in (self.TYPE_GLOBAL, self.TYPE_LOCAL), 'include must either be "global" or "local", got: %s' % name
        self.is_global = name == self.TYPE_GLOBAL
        self.include_type = name
        self.header_name = defn

    def emit_definition(self):
        if self.is_global:
            print '#include <%s>' % self.header_name
        else:
            print '#include "%s"' % self.header_name

    def emit_marshall_declaration(self):
        pass
    def emit_marshall_definition(self):
        pass

def load(file_obj):
    defs = yaml.compose(file_obj)
    assert type(defs) == yaml.nodes.MappingNode
    file_obj.close()

    kinds = {
        'include': KindInclude,
        'struct': KindStruct,
        'const': KindConst,
        'define': KindDefine,
        'enum': KindEnum,
    }

    defines = []
    for kind, kind_def in defs.value:
        cls = kinds[kind.value]
        for data_name, data_def in kind_def.value:
            data_name = data_name.value
            defines.append(cls(data_name, data_def.value))
    return defines

def emit_h(filename):
    defines = load(file(filename))
    def_name = os.path.splitext(os.path.basename(filename))[0]
    hdr_macro_name = '%s_DEF_H' % def_name.upper()

    print '#ifndef %s' % hdr_macro_name
    print '#define %s' % hdr_macro_name
    print

    for defn in defines:
        defn.emit_definition()
        print

    # Emit marshalling function definitions, only for structs
    # Marshalling is not really done right now, we dump the raw structs to disk as it is simpler
    #for defn in defines:
    #    defn.emit_marshall_declaration()
    #    defn.emit_marshall_definition()

    print '#endif'

if __name__ == '__main__':
    import cgitb
    cgitb.enable(format='text')
    emit_h(sys.argv[1])
