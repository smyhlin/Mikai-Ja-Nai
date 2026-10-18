#pragma once

#include <initializer_list>

#include <QByteArray>
#include <QtGlobal>

namespace mikai::tests::fixtures {
namespace detail {

inline void appendVarint(QByteArray& out, quint64 value) {
    while (value >= 0x80) {
        out.append(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.append(static_cast<char>(value));
}

inline void appendTag(QByteArray& out, quint32 field, quint32 wireType) {
    appendVarint(out, (static_cast<quint64>(field) << 3) | wireType);
}

inline void appendInteger(QByteArray& out, quint32 field, quint64 value) {
    appendTag(out, field, 0);
    appendVarint(out, value);
}

inline void appendBytes(QByteArray& out, quint32 field, const QByteArray& value) {
    appendTag(out, field, 2);
    appendVarint(out, static_cast<quint64>(value.size()));
    out.append(value);
}

inline void appendString(QByteArray& out, quint32 field, const char* value) {
    appendBytes(out, field, QByteArray(value));
}

inline QByteArray tensorValueInfo(const char* name) {
    QByteArray shape;
    for (const quint64 size : {1, 3, 4, 4}) {
        QByteArray dimension;
        appendInteger(dimension, 1, size);  // Dimension.dim_value
        appendBytes(shape, 1, dimension);    // TensorShapeProto.dim
    }

    QByteArray tensorType;
    appendInteger(tensorType, 1, 1);         // TensorProto.FLOAT
    appendBytes(tensorType, 2, shape);      // TensorTypeProto.shape

    QByteArray type;
    appendBytes(type, 1, tensorType);       // TypeProto.tensor_type

    QByteArray valueInfo;
    appendString(valueInfo, 1, name);       // ValueInfoProto.name
    appendBytes(valueInfo, 2, type);        // ValueInfoProto.type
    return valueInfo;
}

}  // namespace detail

/// A real ONNX ModelProto containing a 4D FLOAT Identity graph, X -> Y.
inline QByteArray makeTestIdentityOnnxGraph() {
    QByteArray node;
    detail::appendString(node, 1, "X");
    detail::appendString(node, 2, "Y");
    detail::appendString(node, 4, "Identity");

    QByteArray graph;
    detail::appendBytes(graph, 1, node);  // GraphProto.node
    detail::appendString(graph, 2, "g");
    detail::appendBytes(graph, 11, detail::tensorValueInfo("X"));
    detail::appendBytes(graph, 12, detail::tensorValueInfo("Y"));

    QByteArray opset;
    detail::appendInteger(opset, 2, 19);  // OperatorSetIdProto.version

    QByteArray model;
    detail::appendInteger(model, 1, 8);   // ModelProto.ir_version
    detail::appendString(model, 2, "mikai-test");
    detail::appendBytes(model, 7, graph); // ModelProto.graph
    detail::appendBytes(model, 8, opset); // ModelProto.opset_import
    return model;
}

}  // namespace mikai::tests::fixtures
