# Copyright 2026 Zenzen IoT
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from pathlib import Path
from typing import List

from rosidl_parser.definition import AbstractNestedType
from rosidl_parser.definition import AbstractSequence
from rosidl_parser.definition import AbstractString
from rosidl_parser.definition import AbstractWString
from rosidl_parser.definition import Action
from rosidl_parser.definition import Array
from rosidl_parser.definition import BasicType
from rosidl_parser.definition import BoundedSequence
from rosidl_parser.definition import Message
from rosidl_parser.definition import NamespacedType
from rosidl_parser.definition import Service
from rosidl_pycommon import convert_camel_case_to_lower_case_underscore
from rosidl_pycommon import generate_files


_PRIMITIVES = {
    'boolean': ('zidl_cdr_write_bool', 'zidl_cdr_read_bool', 'bool'),
    'octet': ('zidl_cdr_write_u8', 'zidl_cdr_read_u8', 'uint8_t'),
    'char': ('zidl_cdr_write_u8', 'zidl_cdr_read_u8', 'uint8_t'),
    'wchar': ('zidl_cdr_write_u16', 'zidl_cdr_read_u16', 'uint16_t'),
    'float': ('zidl_cdr_write_f32', 'zidl_cdr_read_f32', 'float'),
    'double': ('zidl_cdr_write_f64', 'zidl_cdr_read_f64', 'double'),
    'int8': ('zidl_cdr_write_i8', 'zidl_cdr_read_i8', 'int8_t'),
    'uint8': ('zidl_cdr_write_u8', 'zidl_cdr_read_u8', 'uint8_t'),
    'int16': ('zidl_cdr_write_i16', 'zidl_cdr_read_i16', 'int16_t'),
    'uint16': ('zidl_cdr_write_u16', 'zidl_cdr_read_u16', 'uint16_t'),
    'int32': ('zidl_cdr_write_i32', 'zidl_cdr_read_i32', 'int32_t'),
    'uint32': ('zidl_cdr_write_u32', 'zidl_cdr_read_u32', 'uint32_t'),
    'int64': ('zidl_cdr_write_i64', 'zidl_cdr_read_i64', 'int64_t'),
    'uint64': ('zidl_cdr_write_u64', 'zidl_cdr_read_u64', 'uint64_t'),
}


def generate_cpp(generator_arguments_file: str) -> List[str]:
    mapping = {
        'idl__type_support.cpp.em':
            'detail/dds_zzdds/%s__type_support.cpp',
        'idl__rosidl_typesupport_zzdds_cpp.hpp.em':
            'detail/%s__rosidl_typesupport_zzdds_cpp.hpp',
    }
    return generate_files(generator_arguments_file, mapping)


def generate_c(generator_arguments_file: str) -> List[str]:
    mapping = {
        'idl__type_support_c.cpp.em':
            'detail/dds_zzdds_c/%s__type_support.cpp',
        'idl__rosidl_typesupport_zzdds_c.h.em':
            'detail/%s__rosidl_typesupport_zzdds_c.h',
    }
    return generate_files(generator_arguments_file, mapping)


def all_messages(content):
    messages = list(content.get_elements_of_type(Message))
    services = list(content.get_elements_of_type(Service))
    for action in content.get_elements_of_type(Action):
        messages.extend([
            action.goal, action.result, action.feedback,
            action.feedback_message])
        services.extend([action.send_goal_service, action.get_result_service])
    for service in services:
        messages.extend([
            service.request_message,
            service.response_message,
            service.event_message])
    return messages


def _cpp_name(type_):
    return '::' + '::'.join(type_.namespaced_name())


def _detail_namespace(type_):
    parts = list(type_.namespaces)
    return '::' + '::'.join(parts + ['typesupport_zzdds_cpp'])


def _nested_header(type_):
    namespaces = list(type_.namespaces)
    stem = convert_camel_case_to_lower_case_underscore(type_.name)
    return '/'.join(
        [namespaces[0], namespaces[-1], 'detail',
         stem + '__rosidl_typesupport_zzdds_cpp.hpp'])


def _nested_types(type_):
    if isinstance(type_, AbstractNestedType):
        type_ = type_.value_type
    return [type_] if isinstance(type_, NamespacedType) else []


def nested_headers(messages):
    result = set()
    local_types = {
        tuple(message.structure.namespaced_type.namespaced_name())
        for message in messages
    }
    for message in messages:
        for member in message.structure.members:
            for type_ in _nested_types(member.type):
                if tuple(type_.namespaced_name()) not in local_types:
                    result.add(_nested_header(type_))
    return sorted(result)


def _write_value(type_, expression, indent):
    pad = ' ' * indent
    if isinstance(type_, BasicType):
        if type_.typename == 'long double':
            raise RuntimeError(
                'zzdds static type support does not yet support IDL long double')
        write_fn, _, ctype = _PRIMITIVES[type_.typename]
        return [
            f'{pad}if ({write_fn}(writer, static_cast<{ctype}>({expression})) != '
            'ZIDL_CDR_OK) { return false; }']
    if isinstance(type_, AbstractString):
        lines = [
            f'{pad}if ({expression}.size() > UINT32_MAX) {{ return false; }}',
        ]
        if hasattr(type_, 'maximum_size'):
            lines.append(
                f'{pad}if ({expression}.size() > {type_.maximum_size}u) '
                '{ return false; }')
        lines.append(
            f'{pad}if (zidl_cdr_write_string(writer, {expression}.data(), '
            f'static_cast<uint32_t>({expression}.size())) != ZIDL_CDR_OK) '
            '{ return false; }')
        return lines
    if isinstance(type_, AbstractWString):
        lines = [
            f'{pad}if ({expression}.size() > UINT32_MAX) {{ return false; }}',
        ]
        if hasattr(type_, 'maximum_size'):
            lines.append(
                f'{pad}if ({expression}.size() > {type_.maximum_size}u) '
                '{ return false; }')
        lines.append(
            f'{pad}if (zidl_cdr_write_wstring(writer, '
            f'reinterpret_cast<const uint16_t *>({expression}.data()), '
            f'static_cast<uint32_t>({expression}.size())) != ZIDL_CDR_OK) '
            '{ return false; }')
        return lines
    if isinstance(type_, NamespacedType):
        return [
            f'{pad}if (!{_detail_namespace(type_)}::cdr_serialize('
            f'{expression}, writer)) {{ return false; }}']
    if isinstance(type_, Array):
        lines = [f'{pad}for (const auto & element : {expression}) {{']
        lines.extend(_write_value(type_.value_type, 'element', indent + 2))
        lines.append(f'{pad}}}')
        return lines
    if isinstance(type_, AbstractSequence):
        lines = [
            f'{pad}if ({expression}.size() > UINT32_MAX) {{ return false; }}',
        ]
        if isinstance(type_, BoundedSequence):
            lines.append(
                f'{pad}if ({expression}.size() > {type_.maximum_size}u) '
                '{ return false; }')
        lines.extend([
            f'{pad}if (zidl_cdr_write_u32(writer, '
            f'static_cast<uint32_t>({expression}.size())) != ZIDL_CDR_OK) '
            '{ return false; }',
            f'{pad}for (const auto & element : {expression}) {{',
        ])
        lines.extend(_write_value(type_.value_type, 'element', indent + 2))
        lines.append(f'{pad}}}')
        return lines
    raise RuntimeError(f'unsupported ROS IDL field type: {type_!r}')


def serialize_members(message, indent=2, keys_only=False):
    lines = []
    for member in message.structure.members:
        if keys_only and not member.has_annotation('key'):
            continue
        lines.extend(_write_value(member.type, f'ros_message.{member.name}', indent))
    return '\n'.join(lines)


def filter_field_cases(message, expression='ros_message', c_layout=False, indent=2):
    """Generate top-level scalar/string lookup cases for zzdds CFT evaluation."""
    pad = ' ' * indent
    lines = []
    floating = {'float', 'double'}
    for member in message.structure.members:
        type_ = member.type
        if not isinstance(type_, (BasicType, AbstractString)):
            continue
        value = f'{expression}.{member.name}'
        lines.append(
            f'{pad}if (field_len == {len(member.name)}u && '
            f'std::memcmp(field, "{member.name}", {len(member.name)}u) == 0) {{')
        if isinstance(type_, BasicType):
            if type_.typename == 'long double':
                lines.append(f'{pad}  return false;')
            elif type_.typename in floating:
                lines.extend([
                    f'{pad}  out->kind = {"3" if type_.typename == "float" else "1"};',
                    f'{pad}  out->f = static_cast<double>({value});',
                    f'{pad}  return true;',
                ])
            else:
                lines.extend([
                    f'{pad}  out->kind = 0;',
                    f'{pad}  out->i = static_cast<int64_t>({value});',
                    f'{pad}  return true;',
                ])
        elif c_layout:
            lines.extend([
                f'{pad}  if ({value}.size > scratch_len) {{ return false; }}',
                f'{pad}  std::memcpy(scratch, {value}.data, {value}.size);',
                f'{pad}  out->kind = 2;',
                f'{pad}  out->s_ptr = scratch;',
                f'{pad}  out->s_len = {value}.size;',
                f'{pad}  return true;',
            ])
        else:
            lines.extend([
                f'{pad}  if ({value}.size() > scratch_len) {{ return false; }}',
                f'{pad}  std::memcpy(scratch, {value}.data(), {value}.size());',
                f'{pad}  out->kind = 2;',
                f'{pad}  out->s_ptr = scratch;',
                f'{pad}  out->s_len = {value}.size();',
                f'{pad}  return true;',
            ])
        lines.append(f'{pad}}}')
    lines.append(f'{pad}return false;')
    return '\n'.join(lines)


def _read_value(type_, expression, indent, counter):
    pad = ' ' * indent
    number = counter[0]
    counter[0] += 1
    if isinstance(type_, BasicType):
        if type_.typename == 'long double':
            raise RuntimeError(
                'zzdds static type support does not yet support IDL long double')
        _, read_fn, ctype = _PRIMITIVES[type_.typename]
        name = f'value_{number}'
        return [
            f'{pad}{ctype} {name}{{}};',
            f'{pad}if ({read_fn}(reader, &{name}) != ZIDL_CDR_OK) '
            '{ return false; }',
            f'{pad}{expression} = {name};',
        ]
    if isinstance(type_, AbstractString):
        data = f'string_data_{number}'
        size = f'string_size_{number}'
        lines = [
            f'{pad}const char * {data} = nullptr;',
            f'{pad}uint32_t {size} = 0;',
            f'{pad}if (zidl_cdr_read_string_zerocopy(reader, &{data}, &{size}) '
            '!= ZIDL_CDR_OK) { return false; }',
        ]
        if hasattr(type_, 'maximum_size'):
            lines.append(
                f'{pad}if ({size} > {type_.maximum_size}u) {{ return false; }}')
        lines.append(f'{pad}{expression}.assign({data}, {size});')
        return lines
    if isinstance(type_, AbstractWString):
        data = f'wstring_data_{number}'
        size = f'wstring_size_{number}'
        lines = [
            f'{pad}uint16_t * {data} = nullptr;',
            f'{pad}uint32_t {size} = 0;',
            f'{pad}if (zidl_cdr_read_wstring(reader, &{data}, &{size}) '
            '!= ZIDL_CDR_OK) { return false; }',
        ]
        if hasattr(type_, 'maximum_size'):
            lines.extend([
                f'{pad}if ({size} > {type_.maximum_size}u) {{',
                f'{pad}  zidl_cdr_free_wstr({data});',
                f'{pad}  return false;',
                f'{pad}}}',
            ])
        lines.extend([
            f'{pad}try {{',
            f'{pad}  {expression}.assign('
            f'reinterpret_cast<const char16_t *>({data}), {size});',
            f'{pad}}} catch (...) {{',
            f'{pad}  zidl_cdr_free_wstr({data});',
            f'{pad}  throw;',
            f'{pad}}}',
            f'{pad}zidl_cdr_free_wstr({data});',
        ])
        return lines
    if isinstance(type_, NamespacedType):
        return [
            f'{pad}if (!{_detail_namespace(type_)}::cdr_deserialize('
            f'reader, {expression})) {{ return false; }}']
    if isinstance(type_, Array):
        lines = [f'{pad}for (auto & element : {expression}) {{']
        lines.extend(_read_value(type_.value_type, 'element', indent + 2, counter))
        lines.append(f'{pad}}}')
        return lines
    if isinstance(type_, AbstractSequence):
        size = f'sequence_size_{number}'
        lines = [
            f'{pad}uint32_t {size} = 0;',
            f'{pad}if (zidl_cdr_read_u32(reader, &{size}) != ZIDL_CDR_OK) '
            '{ return false; }',
            # Every valid serialized element occupies at least one byte. This
            # prevents a corrupt length from causing an allocation larger than
            # the remaining payload before type-specific reads validate it.
            f'{pad}if (static_cast<size_t>({size}) > '
            'zidl_cdr_remaining(reader)) { return false; }',
        ]
        if isinstance(type_, BoundedSequence):
            lines.append(
                f'{pad}if ({size} > {type_.maximum_size}u) {{ return false; }}')
        index = f'sequence_index_{number}'
        lines.extend([
            f'{pad}{expression}.resize({size});',
            f'{pad}for (size_t {index} = 0; {index} < {size}; ++{index}) {{',
        ])
        lines.extend(_read_value(
            type_.value_type, f'{expression}[{index}]', indent + 2, counter))
        lines.append(f'{pad}}}')
        return lines
    raise RuntimeError(f'unsupported ROS IDL field type: {type_!r}')


def deserialize_members(message, indent=2):
    lines = []
    counter = [0]
    for member in message.structure.members:
        lines.extend(_read_value(
            member.type, f'ros_message.{member.name}', indent, counter))
    return '\n'.join(lines)


def has_key(message):
    return message.structure.has_any_member_with_annotation('key')


def c_symbol(type_):
    return '__'.join(type_.namespaced_name())


def dds_type_name(type_):
    namespaces = list(type_.namespaces)
    return '::'.join(namespaces + ['dds_', type_.name + '_'])


def header_guard(package_name, interface_path):
    parts = [package_name] + list(Path(interface_path).parts)
    stem = parts[-1].replace('.', '__')
    return '__'.join(parts[:-1] + [stem, 'ROSIDL_TYPESUPPORT_ZZDDS_CPP_HPP_']).upper()


def c_header_guard(package_name, interface_path):
    parts = [package_name] + list(Path(interface_path).parts)
    stem = parts[-1].replace('.', '__')
    return '__'.join(parts[:-1] + [stem, 'ROSIDL_TYPESUPPORT_ZZDDS_C_H_']).upper()


def _c_name(type_):
    return '__'.join(type_.namespaced_name())


def _c_detail_namespace(type_):
    return '::' + '::'.join(list(type_.namespaces) + ['typesupport_zzdds_c'])


def _c_write_value(type_, expression, indent):
    pad = ' ' * indent
    if isinstance(type_, BasicType):
        if type_.typename == 'long double':
            raise RuntimeError('zzdds static type support does not support IDL long double')
        write_fn, _, ctype = _PRIMITIVES[type_.typename]
        return [
            f'{pad}if ({write_fn}(writer, static_cast<{ctype}>({expression})) != '
            'ZIDL_CDR_OK) { return false; }']
    if isinstance(type_, AbstractString):
        lines = [f'{pad}if ({expression}.size > UINT32_MAX) {{ return false; }}']
        if hasattr(type_, 'maximum_size'):
            lines.append(
                f'{pad}if ({expression}.size > {type_.maximum_size}u) '
                '{ return false; }')
        lines.append(
            f'{pad}if (zidl_cdr_write_string(writer, {expression}.data, '
            f'static_cast<uint32_t>({expression}.size)) != ZIDL_CDR_OK) '
            '{ return false; }')
        return lines
    if isinstance(type_, AbstractWString):
        lines = [f'{pad}if ({expression}.size > UINT32_MAX) {{ return false; }}']
        if hasattr(type_, 'maximum_size'):
            lines.append(
                f'{pad}if ({expression}.size > {type_.maximum_size}u) '
                '{ return false; }')
        lines.append(
            f'{pad}if (zidl_cdr_write_wstring(writer, {expression}.data, '
            f'static_cast<uint32_t>({expression}.size)) != ZIDL_CDR_OK) '
            '{ return false; }')
        return lines
    if isinstance(type_, NamespacedType):
        return [
            f'{pad}if (!{_c_detail_namespace(type_)}::cdr_serialize('
            f'{expression}, writer)) {{ return false; }}']
    if isinstance(type_, Array):
        lines = [f'{pad}for (const auto & element : {expression}) {{']
        lines.extend(_c_write_value(type_.value_type, 'element', indent + 2))
        lines.append(f'{pad}}}')
        return lines
    if isinstance(type_, AbstractSequence):
        lines = [
            f'{pad}if ({expression}.size > UINT32_MAX) {{ return false; }}']
        if isinstance(type_, BoundedSequence):
            lines.append(
                f'{pad}if ({expression}.size > {type_.maximum_size}u) '
                '{ return false; }')
        lines.extend([
            f'{pad}if (zidl_cdr_write_u32(writer, '
            f'static_cast<uint32_t>({expression}.size)) != ZIDL_CDR_OK) '
            '{ return false; }',
            f'{pad}for (size_t i = 0; i < {expression}.size; ++i) {{',
        ])
        lines.extend(_c_write_value(
            type_.value_type, f'{expression}.data[i]', indent + 2))
        lines.append(f'{pad}}}')
        return lines
    raise RuntimeError(f'unsupported ROS C IDL field type: {type_!r}')


def c_serialize_members(message, indent=2, keys_only=False):
    lines = []
    for member in message.structure.members:
        if keys_only and not member.has_annotation('key'):
            continue
        lines.extend(_c_write_value(
            member.type, f'ros_message.{member.name}', indent))
    return '\n'.join(lines)


def _c_sequence_functions(type_):
    if isinstance(type_, BasicType):
        names = {
            'boolean': 'bool', 'octet': 'byte', 'char': 'uint8',
            'wchar': 'uint16', 'float': 'float32', 'double': 'float64',
            'int8': 'int8', 'uint8': 'uint8', 'int16': 'int16',
            'uint16': 'uint16', 'int32': 'int32', 'uint32': 'uint32',
            'int64': 'int64', 'uint64': 'uint64',
        }
        base = 'rosidl_runtime_c__' + names[type_.typename] + '__Sequence'
    elif isinstance(type_, AbstractString):
        base = 'rosidl_runtime_c__String__Sequence'
    elif isinstance(type_, AbstractWString):
        base = 'rosidl_runtime_c__U16String__Sequence'
    elif isinstance(type_, NamespacedType):
        base = _c_name(type_) + '__Sequence'
    else:
        raise RuntimeError(f'unsupported ROS C sequence element: {type_!r}')
    return base + '__fini', base + '__init'


def _c_read_value(type_, expression, indent, counter):
    pad = ' ' * indent
    number = counter[0]
    counter[0] += 1
    if isinstance(type_, BasicType):
        if type_.typename == 'long double':
            raise RuntimeError('zzdds static type support does not support IDL long double')
        _, read_fn, ctype = _PRIMITIVES[type_.typename]
        name = f'value_{number}'
        return [
            f'{pad}{ctype} {name}{{}};',
            f'{pad}if ({read_fn}(reader, &{name}) != ZIDL_CDR_OK) '
            '{ return false; }',
            f'{pad}{expression} = {name};',
        ]
    if isinstance(type_, AbstractString):
        data = f'string_data_{number}'
        size = f'string_size_{number}'
        lines = [
            f'{pad}const char * {data} = nullptr;',
            f'{pad}uint32_t {size} = 0;',
            f'{pad}if (zidl_cdr_read_string_zerocopy(reader, &{data}, &{size}) '
            '!= ZIDL_CDR_OK) { return false; }',
        ]
        if hasattr(type_, 'maximum_size'):
            lines.append(
                f'{pad}if ({size} > {type_.maximum_size}u) {{ return false; }}')
        lines.append(
            f'{pad}if (!rosidl_runtime_c__String__assignn('
            f'&{expression}, {data}, {size})) {{ return false; }}')
        return lines
    if isinstance(type_, AbstractWString):
        data = f'wstring_data_{number}'
        size = f'wstring_size_{number}'
        lines = [
            f'{pad}uint16_t * {data} = nullptr;',
            f'{pad}uint32_t {size} = 0;',
            f'{pad}if (zidl_cdr_read_wstring(reader, &{data}, &{size}) '
            '!= ZIDL_CDR_OK) { return false; }',
        ]
        if hasattr(type_, 'maximum_size'):
            lines.extend([
                f'{pad}if ({size} > {type_.maximum_size}u) {{',
                f'{pad}  zidl_cdr_free_wstr({data});',
                f'{pad}  return false;', f'{pad}}}',
            ])
        lines.extend([
            f'{pad}const bool assigned_{number} = rosidl_runtime_c__U16String__assignn('
            f'&{expression}, {data}, {size});',
            f'{pad}zidl_cdr_free_wstr({data});',
            f'{pad}if (!assigned_{number}) {{ return false; }}',
        ])
        return lines
    if isinstance(type_, NamespacedType):
        return [
            f'{pad}if (!{_c_detail_namespace(type_)}::cdr_deserialize('
            f'reader, {expression})) {{ return false; }}']
    if isinstance(type_, Array):
        lines = [f'{pad}for (auto & element : {expression}) {{']
        lines.extend(_c_read_value(type_.value_type, 'element', indent + 2, counter))
        lines.append(f'{pad}}}')
        return lines
    if isinstance(type_, AbstractSequence):
        size = f'sequence_size_{number}'
        fini, init = _c_sequence_functions(type_.value_type)
        lines = [
            f'{pad}uint32_t {size} = 0;',
            f'{pad}if (zidl_cdr_read_u32(reader, &{size}) != ZIDL_CDR_OK) '
            '{ return false; }',
            f'{pad}if (static_cast<size_t>({size}) > '
            'zidl_cdr_remaining(reader)) { return false; }',
        ]
        if isinstance(type_, BoundedSequence):
            lines.append(
                f'{pad}if ({size} > {type_.maximum_size}u) {{ return false; }}')
        lines.extend([
            f'{pad}{fini}(&{expression});',
            f'{pad}if (!{init}(&{expression}, {size})) {{ return false; }}',
            f'{pad}for (size_t i = 0; i < {size}; ++i) {{',
        ])
        lines.extend(_c_read_value(
            type_.value_type, f'{expression}.data[i]', indent + 2, counter))
        lines.append(f'{pad}}}')
        return lines
    raise RuntimeError(f'unsupported ROS C IDL field type: {type_!r}')


def c_deserialize_members(message, indent=2):
    lines = []
    counter = [0]
    for member in message.structure.members:
        lines.extend(_c_read_value(
            member.type, f'ros_message.{member.name}', indent, counter))
    return '\n'.join(lines)
