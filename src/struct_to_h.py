#!/usr/bin/env python

import yaml
import sys
import os

def type_decode(field_def):
    type_name = field_def
    type_param = None
    if type(field_def) == dict:
        assert len(field_def.keys()) == 1
        type_name = field_def.keys()[0]
        type_param = field_def[type_name]
    return (type_name, type_param)

def type_definition(field_def):
    type_name, type_param = type_decode(field_def)

    if type_name == 'tribool':
        return ('tribool', '')
    if type_name == 'int':
        return ('int', '')
    if type_name == 'bool':
        return ('bool', '')
    if type_name == 'string':
        return ('char', '[%d]' % type_param)

    assert False, "should not reach here, unknown type %s with param %s" % (type_name, type_param)


defs = yaml.load(file(sys.argv[1]))

def_name = os.path.splitext(os.path.basename(sys.argv[1]))[0]
hdr_macro_name = '%s_DEF_H' % def_name.upper()

print '#ifndef %s' % hdr_macro_name
print '#define %s' % hdr_macro_name

for struct_name, struct in defs.iteritems():
    struct_name_full = '%s_t' % struct_name
    print 'typedef struct %s {' % struct_name_full
    for field_name, field_def in struct.iteritems():
        type_prefix, type_suffix = type_definition(field_def)
        print '\t%s %s%s;' % (type_prefix, field_name, type_suffix)
    print '} %s;' % struct_name_full

print '#endif'
