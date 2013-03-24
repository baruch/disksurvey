#!/usr/bin/env python

import yaml
import sys
import os

class TypeBool:
    def __init__(self, params):
        pass

    def define(self, name):
        return 'bool %s' % name

class TypeInt:
    def __init__(self, params):
        pass

    def define(self, name):
        return 'int %s' % name

class TypeArray:
    def __init__(self, params):
        self.array_len = from_map(params, 'len')
        self.array_type = from_map(params, 'array_type')

    def define(self, name):
        return '%s %s[%s]' % (self.array_type, name, self.array_len)

class TypeString:
    def __init__(self, params):
        self.str_len = from_map(params, 'len')

    def define(self, name):
        return 'char %s[%s]' % (name, self.str_len)

class TypeEnum:
    def __init__(self, params):
        # An enum can come with no prefix
        self.prefix = from_map_default(params, 'prefix', '')
        values = []
        for key, val in from_map(params, 'values'):
            values.append(key.value)
        self.values  = values

    def define(self, name):
        s = 'enum %s {\n' % name
        for value in self.values:
            s += '\t%s%s,\n' % (self.prefix, value)
        return s + '}'

class TypeTribool:
    def __init__(self, params):
        pass

    def define(self, name):
        return 'tribool_e %s' % name

types = {
    'bool': TypeBool,
    'int': TypeInt,
    'array': TypeArray,
    'string': TypeString,
    'enum': TypeEnum,
    'tribool': TypeTribool,
}

def from_map(mapping, key_name):
    for key, val in mapping.value:
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

def type_definition(field_def):
    type_name = from_map(field_def, 'type')
    t = types.get(type_name, None)
    assert t is not None, "Type '%s' is unknown, field_def=%s" % (type_name, field_def)
    return t(field_def)

def handle_kind_struct(struct_name, struct):
    struct_name_full = '%s_t' % struct_name
    print 'typedef struct %s {' % struct_name_full
    for field_name, field_def in struct.value:
        t = type_definition(field_def)
        print '\t%s;' % t.define(field_name.value)
    print '} %s;' % struct_name_full
    print

def handle_kind_const(const_name, const_def):
    t = type_definition(const_def)
    print 'const %s = %s;' % (t.define(const_name), from_map(const_def, 'value'))

def handle_kind_define(define_name, define_def):
    print '#define %s %s' % (define_name, from_map(define_def, 'value'))

def handle_kind_enum(enum_name, enum_def):
    enum_name_full = '%s_e' % enum_name
    t = TypeEnum(enum_def)
    print 'typedef %s %s;' % (t.define(enum_name_full), enum_name_full)

def handle_kind_include(include_type, include_def):
    val = include_def.value
    if include_type == 'local':
        print '#include "%s"' % val
    elif include_type == 'global':
        print '#include <%s>' % val
    else:
        raise ValueError('Unknown include type %s, must be global or local' % include_type)

defs = yaml.compose(file(sys.argv[1]))
assert type(defs) == yaml.nodes.MappingNode

def_name = os.path.splitext(os.path.basename(sys.argv[1]))[0]
hdr_macro_name = '%s_DEF_H' % def_name.upper()

print '#ifndef %s' % hdr_macro_name
print '#define %s' % hdr_macro_name
print

kinds = {
    'include': handle_kind_include,
    'struct': handle_kind_struct,
    'const': handle_kind_const,
    'define': handle_kind_define,
    'enum': handle_kind_enum,
}

for kind, kind_def in defs.value:
    func = kinds[kind.value]
    for data_name, data_def in kind_def.value:
        data_name = data_name.value
        func(data_name, data_def)
    print

print '#endif'
