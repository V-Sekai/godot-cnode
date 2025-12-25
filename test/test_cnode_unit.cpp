/*
 * Unit tests for Godot CNode functions
 *
 * Tests core CNode functionality without requiring a full Godot instance
 */

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Include erl_interface for encoding/decoding
extern "C" {
#include "ei.h"
}

// Simple test framework
#define TEST_ASSERT(condition, message)                                        \
	do {                                                                       \
		if (!(condition)) {                                                    \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, message); \
			return false;                                                      \
		}                                                                      \
	} while (0)

#define TEST_RUN(test_func)                       \
	do {                                          \
		printf("Running test: %s\n", #test_func); \
		if (test_func()) {                        \
			printf("  ✓ PASSED\n");               \
			passed++;                             \
		} else {                                  \
			printf("  ✗ FAILED\n");               \
			failed++;                             \
		}                                         \
	} while (0)

static int passed = 0;
static int failed = 0;

// Forward declarations for functions we want to test
// Note: These are static in godot_cnode.cpp, so we'll test them indirectly
// or make them testable by creating test versions

/* Test BERT encoding/decoding */
static bool test_bert_encode_decode_int() {
	ei_x_buff x;
	ei_x_new(&x);

	// Encode version first (required for ei buffers)
	ei_x_encode_version(&x);
	// Encode an integer
	int64_t test_value = 42;
	ei_x_encode_long(&x, test_value);

	// Decode it back
	int index = 0;
	long decoded_value;
	if (ei_decode_version(x.buff, &index, NULL) < 0) {
		ei_x_free(&x);
		return false;
	}

	if (ei_decode_long(x.buff, &index, &decoded_value) < 0) {
		ei_x_free(&x);
		return false;
	}

	ei_x_free(&x);
	TEST_ASSERT(decoded_value == test_value, "Integer encode/decode mismatch");
	return true;
}

static bool test_bert_encode_decode_string() {
	ei_x_buff x;
	ei_x_new(&x);

	// Encode version first
	ei_x_encode_version(&x);
	// Encode a string
	const char *test_string = "hello world";
	ei_x_encode_string(&x, test_string);

	// Decode it back
	int index = 0;
	char decoded_string[256];
	if (ei_decode_version(x.buff, &index, NULL) < 0) {
		ei_x_free(&x);
		return false;
	}

	if (ei_decode_string(x.buff, &index, decoded_string) < 0) {
		ei_x_free(&x);
		return false;
	}

	ei_x_free(&x);
	TEST_ASSERT(strcmp(decoded_string, test_string) == 0, "String encode/decode mismatch");
	return true;
}

static bool test_bert_encode_decode_atom() {
	ei_x_buff x;
	ei_x_new(&x);

	// Encode version first
	ei_x_encode_version(&x);
	// Encode an atom
	const char *test_atom = "test_atom";
	ei_x_encode_atom(&x, test_atom);

	// Decode it back
	int index = 0;
	char decoded_atom[256];
	if (ei_decode_version(x.buff, &index, NULL) < 0) {
		ei_x_free(&x);
		return false;
	}

	if (ei_decode_atom(x.buff, &index, decoded_atom) < 0) {
		ei_x_free(&x);
		return false;
	}

	ei_x_free(&x);
	TEST_ASSERT(strcmp(decoded_atom, test_atom) == 0, "Atom encode/decode mismatch");
	return true;
}

static bool test_bert_encode_decode_tuple() {
	ei_x_buff x;
	ei_x_new(&x);

	// Encode version first
	ei_x_encode_version(&x);
	// Encode a tuple: {atom, int, string}
	ei_x_encode_tuple_header(&x, 3);
	ei_x_encode_atom(&x, "test");
	ei_x_encode_long(&x, 123);
	ei_x_encode_string(&x, "value");

	// Decode it back
	int index = 0;
	int arity;
	char atom[256];
	long int_val;
	char string_val[256];

	if (ei_decode_version(x.buff, &index, NULL) < 0) {
		ei_x_free(&x);
		return false;
	}

	if (ei_decode_tuple_header(x.buff, &index, &arity) < 0) {
		ei_x_free(&x);
		return false;
	}
	TEST_ASSERT(arity == 3, "Tuple arity mismatch");

	if (ei_decode_atom(x.buff, &index, atom) < 0) {
		ei_x_free(&x);
		return false;
	}
	TEST_ASSERT(strcmp(atom, "test") == 0, "Tuple atom mismatch");

	if (ei_decode_long(x.buff, &index, &int_val) < 0) {
		ei_x_free(&x);
		return false;
	}
	TEST_ASSERT(int_val == 123, "Tuple int mismatch");

	if (ei_decode_string(x.buff, &index, string_val) < 0) {
		ei_x_free(&x);
		return false;
	}
	TEST_ASSERT(strcmp(string_val, "value") == 0, "Tuple string mismatch");

	ei_x_free(&x);
	return true;
}

static bool test_bert_encode_decode_list() {
	ei_x_buff x;
	ei_x_new(&x);

	// Encode version first
	ei_x_encode_version(&x);
	// Encode a list: [1, 2, 3]
	ei_x_encode_list_header(&x, 3);
	ei_x_encode_long(&x, 1);
	ei_x_encode_long(&x, 2);
	ei_x_encode_long(&x, 3);
	ei_x_encode_empty_list(&x);

	// Decode it back
	int index = 0;
	int arity;
	long values[3];

	if (ei_decode_version(x.buff, &index, NULL) < 0) {
		ei_x_free(&x);
		return false;
	}

	if (ei_decode_list_header(x.buff, &index, &arity) < 0) {
		ei_x_free(&x);
		return false;
	}
	TEST_ASSERT(arity == 3, "List arity mismatch");

	for (int i = 0; i < 3; i++) {
		if (ei_decode_long(x.buff, &index, &values[i]) < 0) {
			ei_x_free(&x);
			return false;
		}
	}

	TEST_ASSERT(values[0] == 1 && values[1] == 2 && values[2] == 3, "List values mismatch");

	ei_x_free(&x);
	return true;
}

/* Test GenServer message format encoding */
static bool test_genserver_call_format() {
	ei_x_buff x;
	ei_x_new(&x);

	// Encode version first
	ei_x_encode_version(&x);
	// Encode GenServer call: {'$gen_call', {From, Tag}, {call, erlang, node, []}}
	// Simplified: just encode the inner call tuple
	ei_x_encode_tuple_header(&x, 4);
	ei_x_encode_atom(&x, "call");
	ei_x_encode_atom(&x, "erlang");
	ei_x_encode_atom(&x, "node");
	ei_x_encode_list_header(&x, 0);
	ei_x_encode_empty_list(&x);

	// Decode it back
	int index = 0;
	int arity;
	char atom1[256], atom2[256], atom3[256];
	int list_arity;

	if (ei_decode_version(x.buff, &index, NULL) < 0) {
		ei_x_free(&x);
		return false;
	}

	if (ei_decode_tuple_header(x.buff, &index, &arity) < 0) {
		ei_x_free(&x);
		return false;
	}
	TEST_ASSERT(arity == 4, "GenServer call tuple arity mismatch");

	if (ei_decode_atom(x.buff, &index, atom1) < 0) {
		ei_x_free(&x);
		return false;
	}
	TEST_ASSERT(strcmp(atom1, "call") == 0, "GenServer call type mismatch");

	if (ei_decode_atom(x.buff, &index, atom2) < 0) {
		ei_x_free(&x);
		return false;
	}
	TEST_ASSERT(strcmp(atom2, "erlang") == 0, "GenServer call module mismatch");

	if (ei_decode_atom(x.buff, &index, atom3) < 0) {
		ei_x_free(&x);
		return false;
	}
	TEST_ASSERT(strcmp(atom3, "node") == 0, "GenServer call function mismatch");

	if (ei_decode_list_header(x.buff, &index, &list_arity) < 0) {
		ei_x_free(&x);
		return false;
	}
	TEST_ASSERT(list_arity == 0, "GenServer call args list should be empty");

	ei_x_free(&x);
	return true;
}

/* Test GenServer cast format */
static bool test_genserver_cast_format() {
	ei_x_buff x;
	ei_x_new(&x);

	// Encode version first
	ei_x_encode_version(&x);
	// Encode GenServer cast: {'$gen_cast', {cast, godot, test, [args]}}
	ei_x_encode_tuple_header(&x, 2);
	ei_x_encode_atom(&x, "$gen_cast");
	ei_x_encode_tuple_header(&x, 4);
	ei_x_encode_atom(&x, "cast");
	ei_x_encode_atom(&x, "godot");
	ei_x_encode_atom(&x, "test");
	ei_x_encode_list_header(&x, 0);
	ei_x_encode_empty_list(&x);

	// Decode it back
	int index = 0;
	int arity;
	char atom[256];

	if (ei_decode_version(x.buff, &index, NULL) < 0) {
		ei_x_free(&x);
		return false;
	}

	if (ei_decode_tuple_header(x.buff, &index, &arity) < 0) {
		ei_x_free(&x);
		return false;
	}
	TEST_ASSERT(arity == 2, "GenServer cast outer tuple arity mismatch");

	if (ei_decode_atom(x.buff, &index, atom) < 0) {
		ei_x_free(&x);
		return false;
	}
	TEST_ASSERT(strcmp(atom, "$gen_cast") == 0, "GenServer cast type mismatch");

	ei_x_free(&x);
	return true;
}

/* Test error handling */
static bool test_decode_invalid_data() {
	ei_x_buff x;
	ei_x_new(&x);

	// Encode version first
	ei_x_encode_version(&x);
	// Encode valid data
	ei_x_encode_long(&x, 42);

	// Try to decode as wrong type (string instead of long)
	int index = 0;
	char string_buf[256];

	if (ei_decode_version(x.buff, &index, NULL) < 0) {
		ei_x_free(&x);
		return false;
	}

	// This should fail
	int result = ei_decode_string(x.buff, &index, string_buf);
	TEST_ASSERT(result < 0, "Should fail to decode long as string");

	ei_x_free(&x);
	return true;
}

/* Test buffer management */
static bool test_buffer_growth() {
	ei_x_buff x;
	ei_x_new(&x);

	// Encode version first
	ei_x_encode_version(&x);
	// Encode many items to test buffer growth
	for (int i = 0; i < 100; i++) {
		ei_x_encode_long(&x, i);
	}

	// Decode them back
	int index = 0;
	if (ei_decode_version(x.buff, &index, NULL) < 0) {
		ei_x_free(&x);
		return false;
	}

	for (int i = 0; i < 100; i++) {
		long value;
		if (ei_decode_long(x.buff, &index, &value) < 0) {
			ei_x_free(&x);
			return false;
		}
		TEST_ASSERT(value == i, "Buffer growth test value mismatch");
	}

	ei_x_free(&x);
	return true;
}

int main() {
	printf("=== Godot CNode Unit Tests ===\n\n");

	// Initialize ei library
	ei_init();

	// Run tests
	TEST_RUN(test_bert_encode_decode_int);
	TEST_RUN(test_bert_encode_decode_string);
	TEST_RUN(test_bert_encode_decode_atom);
	TEST_RUN(test_bert_encode_decode_tuple);
	TEST_RUN(test_bert_encode_decode_list);
	TEST_RUN(test_genserver_call_format);
	TEST_RUN(test_genserver_cast_format);
	TEST_RUN(test_decode_invalid_data);
	TEST_RUN(test_buffer_growth);

	// Summary
	printf("\n=== Test Summary ===\n");
	printf("Passed: %d\n", passed);
	printf("Failed: %d\n", failed);
	printf("Total:  %d\n", passed + failed);

	if (failed == 0) {
		printf("\n✓ All tests passed!\n");
		return 0;
	} else {
		printf("\n✗ Some tests failed!\n");
		return 1;
	}
}
