// SPDX-License-Identifier: BSD-3-Clause

package bess

import (
	"unicode/utf8"
	"unsafe"

	"google.golang.org/grpc/encoding"
	"google.golang.org/grpc/mem"
	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"

	pb "github.com/krsna1729/bess/sdk/go/controlv2"
)

// channelCodec is the protobuf codec, except that a transaction channel
// acknowledgement is decoded by decodeChannelResponse when it can be. The
// wire format and content type are unchanged.
type channelCodec struct{ encoding.CodecV2 }

var transactionChannelCodec = channelCodec{encoding.GetCodecV2("proto")}

func (c channelCodec) Unmarshal(data mem.BufferSlice, v any) error {
	reply, ok := v.(*pb.TransactionChannelResponse)
	if !ok {
		return c.CodecV2.Unmarshal(data, v)
	}
	frame := data.Materialize()
	if decodeChannelResponse(frame, reply) {
		return nil
	}
	return proto.Unmarshal(frame, reply)
}

// decodedResult holds everything one acknowledged transaction decodes into,
// so that a frame's results come from one allocation.
type decodedResult struct {
	result   pb.TransactionChannelResult
	oneof    pb.TransactionChannelResult_Transaction
	response pb.ApplyTransactionResponse
	record   pb.TransactionRecord
}

// decodeChannelResponse fills a fresh reply from an acknowledgement in which
// every transaction has a record, in a few allocations per frame: strings
// point into the frame, which is this reply's own copy and is never written
// again, and results come from one block. It reports false, leaving the frame
// to proto.Unmarshal, on anything else: a refused frame or transaction, a
// field it does not know, a repeated singular message or invalid UTF-8.
func decodeChannelResponse(frame []byte, reply *pb.TransactionChannelResponse) bool {
	if len(frame) == 0 {
		return false
	}
	s := unsafe.String(unsafe.SliceData(frame), len(frame))
	n := 0
	if !fields(frame, 0, func(num protowire.Number, _ protowire.Type, _ uint64, _ []byte, _ int) bool {
		if num == 2 {
			n++
		}
		return true
	}) {
		return false
	}
	slab := make([]decodedResult, n)
	reply.Results = make([]*pb.TransactionChannelResult, 0, n)
	return fields(frame, 0, func(num protowire.Number, typ protowire.Type, v uint64, val []byte, at int) bool {
		switch {
		case num == 1 && typ == protowire.VarintType:
			reply.AttemptId = v
			return true
		case num == 2 && typ == protowire.BytesType:
			d := &slab[len(reply.Results)]
			if !decodeResult(s, val, at, d) {
				return false
			}
			reply.Results = append(reply.Results, &d.result)
			return true
		}
		return false
	})
}

func decodeResult(s string, msg []byte, base int, d *decodedResult) bool {
	seen := false
	if !fields(msg, base, func(num protowire.Number, typ protowire.Type, v uint64, val []byte, at int) bool {
		switch {
		case num == 1 && typ == protowire.BytesType && !seen:
			seen = true
			return decodeResponse(s, val, at, d)
		case num == 3 && typ == protowire.VarintType:
			return v == 0
		}
		return false
	}) || !seen {
		return false
	}
	d.oneof.Transaction = &d.response
	d.result.Result = &d.oneof
	return true
}

func decodeResponse(s string, msg []byte, base int, d *decodedResult) bool {
	seen := false
	return fields(msg, base, func(num protowire.Number, typ protowire.Type, v uint64, val []byte, at int) bool {
		switch {
		case num == 1 && typ == protowire.BytesType && !seen:
			seen = true
			d.response.Record = &d.record
			return decodeRecord(s, val, at, &d.record)
		case num == 2 && typ == protowire.VarintType:
			d.response.DaemonEpoch = v
			return true
		case num == 3 && typ == protowire.VarintType:
			d.response.Replayed = protowire.DecodeBool(v)
			return true
		}
		return false
	})
}

func decodeRecord(s string, msg []byte, base int, r *pb.TransactionRecord) bool {
	n := 0
	if !fields(msg, base, func(num protowire.Number, _ protowire.Type, _ uint64, _ []byte, _ int) bool {
		if num == 4 {
			n++
		}
		return true
	}) {
		return false
	}
	var ops []pb.TransactionOpResult
	if n > 0 {
		ops = make([]pb.TransactionOpResult, n)
		r.Ops = make([]*pb.TransactionOpResult, 0, n)
	}
	return fields(msg, base, func(num protowire.Number, typ protowire.Type, v uint64, val []byte, at int) bool {
		switch {
		case num == 1 && typ == protowire.BytesType && utf8.Valid(val):
			r.RequestId = s[at : at+len(val)]
			return true
		case num == 2 && typ == protowire.VarintType:
			r.Outcome = pb.TransactionRecord_Outcome(int32(v))
			return true
		case num == 3 && typ == protowire.VarintType:
			r.Generation = v
			return true
		case num == 4 && typ == protowire.BytesType:
			op := &ops[len(r.Ops)]
			r.Ops = append(r.Ops, op)
			return decodeOp(s, val, at, op)
		case num == 5 && typ == protowire.VarintType:
			r.Visibility = pb.TransactionRecord_Visibility(int32(v))
			return true
		}
		return false
	})
}

func decodeOp(s string, msg []byte, base int, op *pb.TransactionOpResult) bool {
	return fields(msg, base, func(num protowire.Number, typ protowire.Type, v uint64, val []byte, at int) bool {
		switch {
		case num == 1 && typ == protowire.VarintType:
			op.Status = pb.TransactionOpResult_Status(int32(v))
			return true
		case num == 2 && typ == protowire.BytesType && utf8.Valid(val):
			op.Error = s[at : at+len(val)]
			return true
		}
		return false
	})
}

// fields calls f for each varint or length-delimited field of msg, which
// starts at offset base of the frame: v is a varint's value, val a
// length-delimited field's bytes and at their offset in the frame. It reports
// false for malformed input, any other wire type, or as soon as f does.
func fields(msg []byte, base int, f func(num protowire.Number, typ protowire.Type, v uint64, val []byte, at int) bool) bool {
	for pos := 0; pos < len(msg); {
		num, typ, l := protowire.ConsumeTag(msg[pos:])
		if l < 0 {
			return false
		}
		pos += l
		var v uint64
		var val []byte
		at := 0
		switch typ {
		case protowire.VarintType:
			v, l = protowire.ConsumeVarint(msg[pos:])
		case protowire.BytesType:
			val, l = protowire.ConsumeBytes(msg[pos:])
			at = base + pos + l - len(val)
		default:
			return false
		}
		if l < 0 || !f(num, typ, v, val, at) {
			return false
		}
		pos += l
	}
	return true
}
