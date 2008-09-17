// Copyright 2007-2008 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <stdlib.h>

#include "v8.h"

#include "api.h"
#include "debug.h"
#include "platform.h"
#include "stub-cache.h"
#include "cctest.h"


using ::v8::internal::EmbeddedVector;
using ::v8::internal::Object;
using ::v8::internal::OS;
using ::v8::internal::Handle;
using ::v8::internal::Heap;
using ::v8::internal::JSGlobalObject;
using ::v8::internal::Code;
using ::v8::internal::Debug;
using ::v8::internal::Debugger;
using ::v8::internal::StepAction;
using ::v8::internal::StepIn;  // From StepAction enum
using ::v8::internal::StepNext;  // From StepAction enum
using ::v8::internal::StepOut;  // From StepAction enum


// Size of temp buffer for formatting small strings.
#define SMALL_STRING_BUFFER_SIZE 80

// --- A d d i t i o n a l   C h e c k   H e l p e r s


// Helper function used by the CHECK_EQ function when given Address
// arguments.  Should not be called directly.
static inline void CheckEqualsHelper(const char* file, int line,
                                     const char* expected_source,
                                     ::v8::internal::Address expected,
                                     const char* value_source,
                                     ::v8::internal::Address value) {
  if (expected != value) {
    V8_Fatal(file, line, "CHECK_EQ(%s, %s) failed\n#   "
                         "Expected: %i\n#   Found: %i",
             expected_source, value_source, expected, value);
  }
}


// Helper function used by the CHECK_NE function when given Address
// arguments.  Should not be called directly.
static inline void CheckNonEqualsHelper(const char* file, int line,
                                        const char* unexpected_source,
                                        ::v8::internal::Address unexpected,
                                        const char* value_source,
                                        ::v8::internal::Address value) {
  if (unexpected == value) {
    V8_Fatal(file, line, "CHECK_NE(%s, %s) failed\n#   Value: %i",
             unexpected_source, value_source, value);
  }
}


// Helper function used by the CHECK function when given code
// arguments.  Should not be called directly.
static inline void CheckEqualsHelper(const char* file, int line,
                                     const char* expected_source,
                                     const Code* expected,
                                     const char* value_source,
                                     const Code* value) {
  if (expected != value) {
    V8_Fatal(file, line, "CHECK_EQ(%s, %s) failed\n#   "
                         "Expected: %p\n#   Found: %p",
             expected_source, value_source, expected, value);
  }
}


static inline void CheckNonEqualsHelper(const char* file, int line,
                                        const char* expected_source,
                                        const Code* expected,
                                        const char* value_source,
                                        const Code* value) {
  if (expected == value) {
    V8_Fatal(file, line, "CHECK_NE(%s, %s) failed\n#   Value: %p",
             expected_source, value_source, value);
  }
}


// --- H e l p e r   C l a s s e s


// Helper class for creating a V8 enviromnent for running tests
class DebugLocalContext {
 public:
  inline DebugLocalContext(
      v8::ExtensionConfiguration* extensions = 0,
      v8::Handle<v8::ObjectTemplate> global_template =
          v8::Handle<v8::ObjectTemplate>(),
      v8::Handle<v8::Value> global_object = v8::Handle<v8::Value>())
      : context_(v8::Context::New(extensions, global_template, global_object)) {
    context_->Enter();
  }
  inline ~DebugLocalContext() {
    context_->Exit();
    context_.Dispose();
  }
  inline v8::Context* operator->() { return *context_; }
  inline v8::Context* operator*() { return *context_; }
  inline bool IsReady() { return !context_.IsEmpty(); }
  void ExposeDebug() {
    // Expose the debug context global object in the global object for testing.
    Debug::Load();
    Handle<JSGlobalObject> global(Handle<JSGlobalObject>::cast(
        v8::Utils::OpenHandle(*context_->Global())));
    Handle<JSGlobalObject> debug_global(JSGlobalObject::cast(
        Debug::debug_context()->global()));
    debug_global->set_security_token(global->security_token());
    Handle<v8::internal::String> debug_string =
        v8::internal::Factory::LookupAsciiSymbol("debug");
    SetProperty(global, debug_string, debug_global, DONT_ENUM);
  }
 private:
  v8::Persistent<v8::Context> context_;
};


// --- H e l p e r   F u n c t i o n s


// Compile and run the supplied source and return the fequested function.
static v8::Local<v8::Function> CompileFunction(DebugLocalContext* env,
                                               const char* source,
                                               const char* function_name) {
  v8::Script::Compile(v8::String::New(source))->Run();
  return v8::Local<v8::Function>::Cast(
      (*env)->Global()->Get(v8::String::New(function_name)));
}

// Helper function that compiles and runs the source.
static v8::Local<v8::Value> CompileRun(const char* source) {
  return v8::Script::Compile(v8::String::New(source))->Run();
}

// Is there any debug info for the function?
static bool HasDebugInfo(v8::Handle<v8::Function> fun) {
  Handle<v8::internal::JSFunction> f = v8::Utils::OpenHandle(*fun);
  Handle<v8::internal::SharedFunctionInfo> shared(f->shared());
  return Debug::HasDebugInfo(shared);
}


// Set a break point in a function and return the associated break point
// number.
static int SetBreakPoint(Handle<v8::internal::JSFunction> fun, int position) {
  static int break_point = 0;
  Handle<v8::internal::SharedFunctionInfo> shared(fun->shared());
  Debug::SetBreakPoint(
      shared, position,
      Handle<Object>(v8::internal::Smi::FromInt(++break_point)));
  return break_point;
}


// Set a break point in a function and return the associated break point
// number.
static int SetBreakPoint(v8::Handle<v8::Function> fun, int position) {
  return SetBreakPoint(v8::Utils::OpenHandle(*fun), position);
}


// Set a break point in a function using the Debug object and return the
// associated break point number.
static int SetBreakPointFromJS(const char* function_name,
                               int line, int position) {
  EmbeddedVector<char, SMALL_STRING_BUFFER_SIZE> buffer;
  OS::SNPrintF(buffer,
               "debug.Debug.setBreakPoint(%s,%d,%d)",
               function_name, line, position);
  buffer[SMALL_STRING_BUFFER_SIZE - 1] = '\0';
  v8::Handle<v8::String> str = v8::String::New(buffer.start());
  return v8::Script::Compile(str)->Run()->Int32Value();
}


// Set a break point in a script using the global Debug object.
static int SetScriptBreakPointFromJS(const char* script_data,
                                     int line, int column) {
  EmbeddedVector<char, SMALL_STRING_BUFFER_SIZE> buffer;
  if (column >= 0) {
    // Column specified set script break point on precise location.
    OS::SNPrintF(buffer,
                 "debug.Debug.setScriptBreakPoint(\"%s\",%d,%d)",
                 script_data, line, column);
  } else {
    // Column not specified set script break point on line.
    OS::SNPrintF(buffer,
                 "debug.Debug.setScriptBreakPoint(\"%s\",%d)",
                 script_data, line);
  }
  buffer[SMALL_STRING_BUFFER_SIZE - 1] = '\0';
  v8::Handle<v8::String> str = v8::String::New(buffer.start());
  return v8::Script::Compile(str)->Run()->Int32Value();
}


// Clear a break point.
static void ClearBreakPoint(int break_point) {
  Debug::ClearBreakPoint(
      Handle<Object>(v8::internal::Smi::FromInt(break_point)));
}


// Clear a break point using the global Debug object.
static void ClearBreakPointFromJS(int break_point_number) {
  EmbeddedVector<char, SMALL_STRING_BUFFER_SIZE> buffer;
  OS::SNPrintF(buffer,
               "debug.Debug.clearBreakPoint(%d)",
               break_point_number);
  buffer[SMALL_STRING_BUFFER_SIZE - 1] = '\0';
  v8::Script::Compile(v8::String::New(buffer.start()))->Run();
}


static void EnableScriptBreakPointFromJS(int break_point_number) {
  EmbeddedVector<char, SMALL_STRING_BUFFER_SIZE> buffer;
  OS::SNPrintF(buffer,
               "debug.Debug.enableScriptBreakPoint(%d)",
               break_point_number);
  buffer[SMALL_STRING_BUFFER_SIZE - 1] = '\0';
  v8::Script::Compile(v8::String::New(buffer.start()))->Run();
}


static void DisableScriptBreakPointFromJS(int break_point_number) {
  EmbeddedVector<char, SMALL_STRING_BUFFER_SIZE> buffer;
  OS::SNPrintF(buffer,
               "debug.Debug.disableScriptBreakPoint(%d)",
               break_point_number);
  buffer[SMALL_STRING_BUFFER_SIZE - 1] = '\0';
  v8::Script::Compile(v8::String::New(buffer.start()))->Run();
}


static void ChangeScriptBreakPointConditionFromJS(int break_point_number,
                                                  const char* condition) {
  EmbeddedVector<char, SMALL_STRING_BUFFER_SIZE> buffer;
  OS::SNPrintF(buffer,
               "debug.Debug.changeScriptBreakPointCondition(%d, \"%s\")",
               break_point_number, condition);
  buffer[SMALL_STRING_BUFFER_SIZE - 1] = '\0';
  v8::Script::Compile(v8::String::New(buffer.start()))->Run();
}


static void ChangeScriptBreakPointIgnoreCountFromJS(int break_point_number,
                                                    int ignoreCount) {
  EmbeddedVector<char, SMALL_STRING_BUFFER_SIZE> buffer;
  OS::SNPrintF(buffer,
               "debug.Debug.changeScriptBreakPointIgnoreCount(%d, %d)",
               break_point_number, ignoreCount);
  buffer[SMALL_STRING_BUFFER_SIZE - 1] = '\0';
  v8::Script::Compile(v8::String::New(buffer.start()))->Run();
}


// Change break on exception.
static void ChangeBreakOnException(bool caught, bool uncaught) {
  Debug::ChangeBreakOnException(v8::internal::BreakException, caught);
  Debug::ChangeBreakOnException(v8::internal::BreakUncaughtException, uncaught);
}


// Change break on exception using the global Debug object.
static void ChangeBreakOnExceptionFromJS(bool caught, bool uncaught) {
  if (caught) {
    v8::Script::Compile(
        v8::String::New("debug.Debug.setBreakOnException()"))->Run();
  } else {
    v8::Script::Compile(
        v8::String::New("debug.Debug.clearBreakOnException()"))->Run();
  }
  if (uncaught) {
    v8::Script::Compile(
        v8::String::New("debug.Debug.setBreakOnUncaughtException()"))->Run();
  } else {
    v8::Script::Compile(
        v8::String::New("debug.Debug.clearBreakOnUncaughtException()"))->Run();
  }
}


// Prepare to step to next break location.
static void PrepareStep(StepAction step_action) {
  Debug::PrepareStep(step_action, 1);
}


// This function is in namespace v8::internal to be friend with class
// v8::internal::Debug.
namespace v8 { namespace internal {  // NOLINT

// Collect the currently debugged functions.
Handle<FixedArray> GetDebuggedFunctions() {
  v8::internal::DebugInfoListNode* node = Debug::debug_info_list_;

  // Find the number of debugged functions.
  int count = 0;
  while (node) {
    count++;
    node = node->next();
  }

  // Allocate array for the debugged functions
  Handle<FixedArray> debugged_functions =
      v8::internal::Factory::NewFixedArray(count);

  // Run through the debug info objects and collect all functions.
  count = 0;
  while (node) {
    debugged_functions->set(count++, *node->debug_info());
    node = node->next();
  }

  return debugged_functions;
}


static Handle<Code> ComputeCallDebugBreak(int argc) {
  CALL_HEAP_FUNCTION(v8::internal::StubCache::ComputeCallDebugBreak(argc),
                     Code);
}

} }  // namespace v8::internal

// Inherit from BreakLocationIterator to get access to protected parts for
// testing.
class TestBreakLocationIterator: public v8::internal::BreakLocationIterator {
 public:
  explicit TestBreakLocationIterator(Handle<v8::internal::DebugInfo> debug_info)
    : BreakLocationIterator(debug_info, v8::internal::SOURCE_BREAK_LOCATIONS) {}
  v8::internal::RelocIterator* it() { return reloc_iterator_; }
  v8::internal::RelocIterator* it_original() {
    return reloc_iterator_original_;
  }
};


// Compile a function, set a break point and check that the call at the break
// location in the code is the expected debug_break function.
void CheckDebugBreakFunction(DebugLocalContext* env,
                             const char* source, const char* name,
                             int position, v8::internal::RelocMode mode,
                             Code* debug_break) {
  // Create function and set the break point.
  Handle<v8::internal::JSFunction> fun = v8::Utils::OpenHandle(
      *CompileFunction(env, source, name));
  int bp = SetBreakPoint(fun, position);

  // Check that the debug break function is as expected.
  Handle<v8::internal::SharedFunctionInfo> shared(fun->shared());
  CHECK(Debug::HasDebugInfo(shared));
  TestBreakLocationIterator it1(Debug::GetDebugInfo(shared));
  it1.FindBreakLocationFromPosition(position);
  CHECK_EQ(mode, it1.it()->rinfo()->rmode());
  if (mode != v8::internal::js_return) {
    CHECK_EQ(debug_break,
             Debug::GetCodeTarget(it1.it()->rinfo()->target_address()));
  } else {
    // TODO(1240753): Make the test architecture independent or split
    // parts of the debugger into architecture dependent files.
    CHECK_EQ(0xE8, *(it1.rinfo()->pc()));
  }

  // Clear the break point and check that the debug break function is no longer
  // there
  ClearBreakPoint(bp);
  CHECK(!Debug::HasDebugInfo(shared));
  CHECK(Debug::EnsureDebugInfo(shared));
  TestBreakLocationIterator it2(Debug::GetDebugInfo(shared));
  it2.FindBreakLocationFromPosition(position);
  CHECK_EQ(mode, it2.it()->rinfo()->rmode());
  if (mode == v8::internal::js_return) {
    // TODO(1240753): Make the test architecture independent or split
    // parts of the debugger into architecture dependent files.
    CHECK_NE(0xE8, *(it2.rinfo()->pc()));
  }
}


// --- D e b u g   E v e n t   H a n d l e r s
// ---
// --- The different tests uses a number of debug event handlers.
// ---


// Source for The JavaScript function which picks out the function name on the
// top frame.
const char* frame_function_name_source =
    "function frame_function_name(exec_state) {"
    "  return exec_state.frame(0).func().name();"
    "}";
v8::Local<v8::Function> frame_function_name;

// Global variable to store the last function hit - used by some tests.
char last_function_hit[80];

// Debug event handler which counts the break points which have been hit.
int break_point_hit_count = 0;
static void DebugEventBreakPointHitCount(v8::DebugEvent event,
                                         v8::Handle<v8::Object> exec_state,
                                         v8::Handle<v8::Object> event_data,
                                         v8::Handle<v8::Value> data) {
  // Count the number of breaks.
  if (event == v8::Break) {
    break_point_hit_count++;
    if (!frame_function_name.IsEmpty()) {
      // Get the name of the function.
      const int argc = 1;
      v8::Handle<v8::Value> argv[argc] = { exec_state };
      v8::Handle<v8::Value> result = frame_function_name->Call(exec_state,
                                                               argc, argv);
      if (result->IsUndefined()) {
        last_function_hit[0] = '\0';
      } else {
        CHECK(result->IsString());
        v8::Handle<v8::String> function_name(result->ToString());
        function_name->WriteAscii(last_function_hit);
      }
    }
  }
}


// Debug event handler which counts a number of events.
int exception_hit_count = 0;
int uncaught_exception_hit_count = 0;

static void DebugEventCounterClear() {
  break_point_hit_count = 0;
  exception_hit_count = 0;
  uncaught_exception_hit_count = 0;
}

static void DebugEventCounter(v8::DebugEvent event,
                              v8::Handle<v8::Object> exec_state,
                              v8::Handle<v8::Object> event_data,
                              v8::Handle<v8::Value> data) {
  // Count the number of breaks.
  if (event == v8::Break) {
    break_point_hit_count++;
  } else if (event == v8::Exception) {
    exception_hit_count++;

    // Check whether the exception was uncaught.
    v8::Local<v8::String> fun_name = v8::String::New("uncaught");
    v8::Local<v8::Function> fun =
        v8::Function::Cast(*event_data->Get(fun_name));
    v8::Local<v8::Value> result = *fun->Call(event_data, 0, NULL);
    if (result->IsTrue()) {
      uncaught_exception_hit_count++;
    }
  }
}


// Debug event handler which evaluates a number of expressions when a break
// point is hit. Each evaluated expression is compared with an expected value.
// For this debug event handler to work the following two global varaibles
// must be initialized.
//   checks: An array of expressions and expected results
//   evaluate_check_function: A JavaScript function (see below)

// Structure for holding checks to do.
struct EvaluateCheck {
  const char* expr;  // An expression to evaluate when a break point is hit.
  v8::Handle<v8::Value> expected;  // The expected result.
};
// Array of checks to do.
struct EvaluateCheck* checks = NULL;
// Source for The JavaScript function which can do the evaluation when a break
// point is hit.
const char* evaluate_check_source =
    "function evaluate_check(exec_state, expr, expected) {"
    "  return exec_state.frame(0).evaluate(expr).value() === expected;"
    "}";
v8::Local<v8::Function> evaluate_check_function;

// The actual debug event described by the longer comment above.
static void DebugEventEvaluate(v8::DebugEvent event,
                               v8::Handle<v8::Object> exec_state,
                               v8::Handle<v8::Object> event_data,
                               v8::Handle<v8::Value> data) {
  if (event == v8::Break) {
    for (int i = 0; checks[i].expr != NULL; i++) {
      const int argc = 3;
      v8::Handle<v8::Value> argv[argc] = { exec_state,
                                           v8::String::New(checks[i].expr),
                                           checks[i].expected };
      v8::Handle<v8::Value> result =
          evaluate_check_function->Call(exec_state, argc, argv);
      if (!result->IsTrue()) {
        v8::String::AsciiValue ascii(checks[i].expected->ToString());
        V8_Fatal(__FILE__, __LINE__, "%s != %s", checks[i].expr, *ascii);
      }
    }
  }
}


// This debug event listener removes a breakpoint in a function
int debug_event_remove_break_point = 0;
static void DebugEventRemoveBreakPoint(v8::DebugEvent event,
                                       v8::Handle<v8::Object> exec_state,
                                       v8::Handle<v8::Object> event_data,
                                       v8::Handle<v8::Value> data) {
  if (event == v8::Break) {
    break_point_hit_count++;
    v8::Handle<v8::Function> fun = v8::Handle<v8::Function>::Cast(data);
    ClearBreakPoint(debug_event_remove_break_point);
  }
}


// Debug event handler which counts break points hit and performs a step
// afterwards.
StepAction step_action = StepIn;  // Step action to perform when stepping.
static void DebugEventStep(v8::DebugEvent event,
                           v8::Handle<v8::Object> exec_state,
                           v8::Handle<v8::Object> event_data,
                           v8::Handle<v8::Value> data) {
  if (event == v8::Break) {
    break_point_hit_count++;
    PrepareStep(step_action);
  }
}


// Debug event handler which counts break points hit and performs a step
// afterwards. For each call the expected function is checked.
// For this debug event handler to work the following two global varaibles
// must be initialized.
//   expected_step_sequence: An array of the expected function call sequence.
//   frame_function_name: A JavaScript function (see below).

// String containing the expected function call sequence. Note: this only works
// if functions have name length of one.
const char* expected_step_sequence = NULL;

// The actual debug event described by the longer comment above.
static void DebugEventStepSequence(v8::DebugEvent event,
                                   v8::Handle<v8::Object> exec_state,
                                   v8::Handle<v8::Object> event_data,
                                   v8::Handle<v8::Value> data) {
  if (event == v8::Break || event == v8::Exception) {
    // Check that the current function is the expected.
    CHECK(break_point_hit_count <
          static_cast<int>(strlen(expected_step_sequence)));
    const int argc = 1;
    v8::Handle<v8::Value> argv[argc] = { exec_state };
    v8::Handle<v8::Value> result = frame_function_name->Call(exec_state,
                                                             argc, argv);
    CHECK(result->IsString());
    v8::String::AsciiValue function_name(result->ToString());
    CHECK_EQ(1, strlen(*function_name));
    CHECK_EQ((*function_name)[0],
              expected_step_sequence[break_point_hit_count]);

    // Perform step.
    break_point_hit_count++;
    PrepareStep(step_action);
  }
}


// Debug event handler which performs a garbage collection.
static void DebugEventBreakPointCollectGarbage(
    v8::DebugEvent event,
    v8::Handle<v8::Object> exec_state,
    v8::Handle<v8::Object> event_data,
    v8::Handle<v8::Value> data) {
  // Perform a garbage collection when break point is hit and continue. Based
  // on the number of break points hit either scavenge or mark compact
  // collector is used.
  if (event == v8::Break) {
    break_point_hit_count++;
    if (break_point_hit_count % 2 == 0) {
      // Scavenge.
      Heap::CollectGarbage(0, v8::internal::NEW_SPACE);
    } else {
      // Mark sweep (and perhaps compact).
      Heap::CollectAllGarbage();
    }
  }
}


// Debug event handler which re-issues a debug break and calls the garbage
// collector to have the heap verified.
static void DebugEventBreak(v8::DebugEvent event,
                            v8::Handle<v8::Object> exec_state,
                            v8::Handle<v8::Object> event_data,
                            v8::Handle<v8::Value> data) {
  if (event == v8::Break) {
    // Count the number of breaks.
    break_point_hit_count++;

    // Run the garbage collector to enforce heap verification if option
    // --verify-heap is set.
    Heap::CollectGarbage(0, v8::internal::NEW_SPACE);

    // Set the break flag again to come back here as soon as possible.
    v8::Debug::DebugBreak();
  }
}


// --- M e s s a g e   C a l l b a c k


// Message callback which counts the number of messages.
int message_callback_count = 0;

static void MessageCallbackCountClear() {
  message_callback_count = 0;
}

static void MessageCallbackCount(v8::Handle<v8::Message> message,
                                 v8::Handle<v8::Value> data) {
  message_callback_count++;
}


// --- T h e   A c t u a l   T e s t s


// Test that the debug break function is the expected one for different kinds
// of break locations.
TEST(DebugStub) {
  using ::v8::internal::Builtins;
  v8::HandleScope scope;
  DebugLocalContext env;

  // TODO(1240753): Make the test architecture independent or split
  // parts of the debugger into architecture dependent files. This
  // part currently disabled as it is not portable between IA32/ARM.
  // Ia32 uses js_return ARM uses exit_js_frame.
#if !defined (__arm__) && !defined(__thumb__)
  CheckDebugBreakFunction(&env,
                          "function f1(){}", "f1",
                          0,
                          v8::internal::js_return,
                          NULL);
#endif
  CheckDebugBreakFunction(&env,
                          "function f2(){x=1;}", "f2",
                          0,
                          v8::internal::code_target,
                          Builtins::builtin(Builtins::StoreIC_DebugBreak));
  CheckDebugBreakFunction(&env,
                          "function f3(){var a=x;}", "f3",
                          0,
                          v8::internal::code_target_context,
                          Builtins::builtin(Builtins::LoadIC_DebugBreak));

// Currently on ICs for keyed store/load on ARM.
#if !defined (__arm__) && !defined(__thumb__)
  CheckDebugBreakFunction(
      &env,
      "function f4(){var index='propertyName'; var a={}; a[index] = 'x';}",
      "f4",
      0,
      v8::internal::code_target,
      Builtins::builtin(Builtins::KeyedStoreIC_DebugBreak));
  CheckDebugBreakFunction(
      &env,
      "function f5(){var index='propertyName'; var a={}; return a[index];}",
      "f5",
      0,
      v8::internal::code_target,
      Builtins::builtin(Builtins::KeyedLoadIC_DebugBreak));
#endif

  // Check the debug break code stubs for call ICs with different number of
  // parameters.
  Handle<Code> debug_break_0 = v8::internal::ComputeCallDebugBreak(0);
  Handle<Code> debug_break_1 = v8::internal::ComputeCallDebugBreak(1);
  Handle<Code> debug_break_4 = v8::internal::ComputeCallDebugBreak(4);

  CheckDebugBreakFunction(&env,
                          "function f4_0(){x();}", "f4_0",
                          0,
                          v8::internal::code_target_context,
                          *debug_break_0);

  CheckDebugBreakFunction(&env,
                          "function f4_1(){x(1);}", "f4_1",
                          0,
                          v8::internal::code_target_context,
                          *debug_break_1);

  CheckDebugBreakFunction(&env,
                          "function f4_4(){x(1,2,3,4);}", "f4_4",
                          0,
                          v8::internal::code_target_context,
                          *debug_break_4);
}


// Test that the debug info in the VM is in sync with the functions being
// debugged.
TEST(DebugInfo) {
  v8::HandleScope scope;
  DebugLocalContext env;
  // Create a couple of functions for the test.
  v8::Local<v8::Function> foo =
      CompileFunction(&env, "function foo(){}", "foo");
  v8::Local<v8::Function> bar =
      CompileFunction(&env, "function bar(){}", "bar");
  // Initially no functions are debugged.
  CHECK_EQ(0, v8::internal::GetDebuggedFunctions()->length());
  CHECK(!HasDebugInfo(foo));
  CHECK(!HasDebugInfo(bar));
  // One function (foo) is debugged.
  int bp1 = SetBreakPoint(foo, 0);
  CHECK_EQ(1, v8::internal::GetDebuggedFunctions()->length());
  CHECK(HasDebugInfo(foo));
  CHECK(!HasDebugInfo(bar));
  // Two functions are debugged.
  int bp2 = SetBreakPoint(bar, 0);
  CHECK_EQ(2, v8::internal::GetDebuggedFunctions()->length());
  CHECK(HasDebugInfo(foo));
  CHECK(HasDebugInfo(bar));
  // One function (bar) is debugged.
  ClearBreakPoint(bp1);
  CHECK_EQ(1, v8::internal::GetDebuggedFunctions()->length());
  CHECK(!HasDebugInfo(foo));
  CHECK(HasDebugInfo(bar));
  // No functions are debugged.
  ClearBreakPoint(bp2);
  CHECK_EQ(0, v8::internal::GetDebuggedFunctions()->length());
  CHECK(!HasDebugInfo(foo));
  CHECK(!HasDebugInfo(bar));
}


// Test that a break point can be set at an IC store location.
TEST(BreakPointICStore) {
  break_point_hit_count = 0;
  v8::HandleScope scope;
  DebugLocalContext env;
  v8::Debug::AddDebugEventListener(DebugEventBreakPointHitCount,
                                   v8::Undefined());
  v8::Script::Compile(v8::String::New("function foo(){bar=0;}"))->Run();
  v8::Local<v8::Function> foo =
      v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("foo")));

  // Run without breakpoints.
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);

  // Run with breakpoint
  int bp = SetBreakPoint(foo, 0);
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);

  // Run without breakpoints.
  ClearBreakPoint(bp);
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointHitCount);
}


// Test that a break point can be set at an IC load location.
TEST(BreakPointICLoad) {
  break_point_hit_count = 0;
  v8::HandleScope scope;
  DebugLocalContext env;
  v8::Debug::AddDebugEventListener(DebugEventBreakPointHitCount,
                                   v8::Undefined());
  v8::Script::Compile(v8::String::New("bar=1"))->Run();
  v8::Script::Compile(v8::String::New("function foo(){var x=bar;}"))->Run();
  v8::Local<v8::Function> foo =
      v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("foo")));

  // Run without breakpoints.
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);

  // Run with breakpoint
  int bp = SetBreakPoint(foo, 0);
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);

  // Run without breakpoints.
  ClearBreakPoint(bp);
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointHitCount);
}


// Test that a break point can be set at an IC call location.
TEST(BreakPointICCall) {
  break_point_hit_count = 0;
  v8::HandleScope scope;
  DebugLocalContext env;
  v8::Debug::AddDebugEventListener(DebugEventBreakPointHitCount,
                                   v8::Undefined());
  v8::Script::Compile(v8::String::New("function bar(){}"))->Run();
  v8::Script::Compile(v8::String::New("function foo(){bar();}"))->Run();
  v8::Local<v8::Function> foo =
      v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("foo")));

  // Run without breakpoints.
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);

  // Run with breakpoint
  int bp = SetBreakPoint(foo, 0);
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);

  // Run without breakpoints.
  ClearBreakPoint(bp);
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointHitCount);
}


// Test that a break point can be set at a return store location.
TEST(BreakPointReturn) {
  break_point_hit_count = 0;
  v8::HandleScope scope;
  DebugLocalContext env;
  v8::Debug::AddDebugEventListener(DebugEventBreakPointHitCount,
                                   v8::Undefined());
  v8::Script::Compile(v8::String::New("function foo(){}"))->Run();
  v8::Local<v8::Function> foo =
      v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("foo")));

  // Run without breakpoints.
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);

  // Run with breakpoint
  int bp = SetBreakPoint(foo, 0);
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);

  // Run without breakpoints.
  ClearBreakPoint(bp);
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointHitCount);
}


static void CallWithBreakPoints(v8::Local<v8::Object> recv,
                                v8::Local<v8::Function> f,
                                int break_point_count,
                                int call_count) {
  break_point_hit_count = 0;
  for (int i = 0; i < call_count; i++) {
    f->Call(recv, 0, NULL);
    CHECK_EQ((i + 1) * break_point_count, break_point_hit_count);
  }
}

// Test GC during break point processing.
TEST(GCDuringBreakPointProcessing) {
  break_point_hit_count = 0;
  v8::HandleScope scope;
  DebugLocalContext env;

  v8::Debug::AddDebugEventListener(DebugEventBreakPointCollectGarbage,
                                   v8::Undefined());
  v8::Local<v8::Function> foo;

  // Test IC store break point with garbage collection.
  foo = CompileFunction(&env, "function foo(){bar=0;}", "foo");
  SetBreakPoint(foo, 0);
  CallWithBreakPoints(env->Global(), foo, 1, 10);

  // Test IC load break point with garbage collection.
  foo = CompileFunction(&env, "bar=1;function foo(){var x=bar;}", "foo");
  SetBreakPoint(foo, 0);
  CallWithBreakPoints(env->Global(), foo, 1, 10);

  // Test IC call break point with garbage collection.
  foo = CompileFunction(&env, "function bar(){};function foo(){bar();}", "foo");
  SetBreakPoint(foo, 0);
  CallWithBreakPoints(env->Global(), foo, 1, 10);

  // Test return break point with garbage collection.
  foo = CompileFunction(&env, "function foo(){}", "foo");
  SetBreakPoint(foo, 0);
  CallWithBreakPoints(env->Global(), foo, 1, 25);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointCollectGarbage);
}


// Call the function three times with different garbage collections in between
// and make sure that the break point survives.
static void CallAndGC(v8::Local<v8::Object> recv, v8::Local<v8::Function> f) {
  break_point_hit_count = 0;

  for (int i = 0; i < 3; i++) {
    // Call function.
    f->Call(recv, 0, NULL);
    CHECK_EQ(1 + i * 3, break_point_hit_count);

    // Scavenge and call function.
    Heap::CollectGarbage(0, v8::internal::NEW_SPACE);
    f->Call(recv, 0, NULL);
    CHECK_EQ(2 + i * 3, break_point_hit_count);

    // Mark sweep (and perhaps compact) and call function.
    Heap::CollectAllGarbage();
    f->Call(recv, 0, NULL);
    CHECK_EQ(3 + i * 3, break_point_hit_count);
  }
}


// Test that a break point can be set at a return store location.
TEST(BreakPointSurviveGC) {
  break_point_hit_count = 0;
  v8::HandleScope scope;
  DebugLocalContext env;

  v8::Debug::AddDebugEventListener(DebugEventBreakPointHitCount,
                                   v8::Undefined());
  v8::Local<v8::Function> foo;

  // Test IC store break point with garbage collection.
  foo = CompileFunction(&env, "function foo(){bar=0;}", "foo");
  SetBreakPoint(foo, 0);
  CallAndGC(env->Global(), foo);

  // Test IC load break point with garbage collection.
  foo = CompileFunction(&env, "bar=1;function foo(){var x=bar;}", "foo");
  SetBreakPoint(foo, 0);
  CallAndGC(env->Global(), foo);

  // Test IC call break point with garbage collection.
  foo = CompileFunction(&env, "function bar(){};function foo(){bar();}", "foo");
  SetBreakPoint(foo, 0);
  CallAndGC(env->Global(), foo);

  // Test return break point with garbage collection.
  foo = CompileFunction(&env, "function foo(){}", "foo");
  SetBreakPoint(foo, 0);
  CallAndGC(env->Global(), foo);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointHitCount);
}


// Test that break points can be set using the global Debug object.
TEST(BreakPointThroughJavaScript) {
  break_point_hit_count = 0;
  v8::HandleScope scope;
  DebugLocalContext env;
  env.ExposeDebug();

  v8::Debug::AddDebugEventListener(DebugEventBreakPointHitCount,
                                   v8::Undefined());
  v8::Script::Compile(v8::String::New("function bar(){}"))->Run();
  v8::Script::Compile(v8::String::New("function foo(){bar();bar();}"))->Run();
  //                                               012345678901234567890
  //                                                         1         2
  // Break points are set at position 3 and 9
  v8::Local<v8::Script> foo = v8::Script::Compile(v8::String::New("foo()"));

  // Run without breakpoints.
  foo->Run();
  CHECK_EQ(0, break_point_hit_count);

  // Run with one breakpoint
  int bp1 = SetBreakPointFromJS("foo", 0, 3);
  foo->Run();
  CHECK_EQ(1, break_point_hit_count);
  foo->Run();
  CHECK_EQ(2, break_point_hit_count);

  // Run with two breakpoints
  int bp2 = SetBreakPointFromJS("foo", 0, 9);
  foo->Run();
  CHECK_EQ(4, break_point_hit_count);
  foo->Run();
  CHECK_EQ(6, break_point_hit_count);

  // Run with one breakpoint
  ClearBreakPointFromJS(bp2);
  foo->Run();
  CHECK_EQ(7, break_point_hit_count);
  foo->Run();
  CHECK_EQ(8, break_point_hit_count);

  // Run without breakpoints.
  ClearBreakPointFromJS(bp1);
  foo->Run();
  CHECK_EQ(8, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointHitCount);

  // Make sure that the break point numbers are consecutive.
  CHECK_EQ(1, bp1);
  CHECK_EQ(2, bp2);
}


// Test that break points can be set using the global Debug object.
TEST(ScriptBreakPointThroughJavaScript) {
  break_point_hit_count = 0;
  v8::HandleScope scope;
  DebugLocalContext env;
  env.ExposeDebug();

  v8::Debug::AddDebugEventListener(DebugEventBreakPointHitCount,
                                   v8::Undefined());
  v8::Script::Compile(v8::String::New("function foo(){bar();bar();}"))->Run();

  v8::Local<v8::String> script = v8::String::New(
    "function f() {\n"
    "  function h() {\n"
    "    a = 0;  // line 2\n"
    "  }\n"
    "  b = 1;  // line 4\n"
    "  return h();\n"
    "}\n"
    "\n"
    "function g() {\n"
    "  function h() {\n"
    "    a = 0;\n"
    "  }\n"
    "  b = 2;  // line 12\n"
    "  h();\n"
    "  b = 3;  // line 14\n"
    "  f();    // line 15\n"
    "}");

  // Compile the script and get the two functions.
  v8::ScriptOrigin origin =
      v8::ScriptOrigin(v8::String::New("test"));
  v8::Script::Compile(script, &origin)->Run();
  v8::Local<v8::Function> f =
      v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("f")));
  v8::Local<v8::Function> g =
      v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("g")));

  // Call f and g without break points.
  break_point_hit_count = 0;
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);
  g->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);

  // Call f and g with break point on line 12.
  int sbp1 = SetScriptBreakPointFromJS("test", 12, 0);
  break_point_hit_count = 0;
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);
  g->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);

  // Remove the break point again.
  break_point_hit_count = 0;
  ClearBreakPointFromJS(sbp1);
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);
  g->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);

  // Call f and g with break point on line 2.
  int sbp2 = SetScriptBreakPointFromJS("test", 2, 0);
  break_point_hit_count = 0;
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);
  g->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);

  // Call f and g with break point on line 2, 4, 12, 14 and 15.
  int sbp3 = SetScriptBreakPointFromJS("test", 4, 0);
  int sbp4 = SetScriptBreakPointFromJS("test", 12, 0);
  int sbp5 = SetScriptBreakPointFromJS("test", 14, 0);
  int sbp6 = SetScriptBreakPointFromJS("test", 15, 0);
  break_point_hit_count = 0;
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);
  g->Call(env->Global(), 0, NULL);
  CHECK_EQ(7, break_point_hit_count);

  // Remove the all the break points again.
  break_point_hit_count = 0;
  ClearBreakPointFromJS(sbp2);
  ClearBreakPointFromJS(sbp3);
  ClearBreakPointFromJS(sbp4);
  ClearBreakPointFromJS(sbp5);
  ClearBreakPointFromJS(sbp6);
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);
  g->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);

  // Now set a function break point
  int bp7 = SetBreakPointFromJS("g", 0, 0);
  g->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);

  // Reload the script and get g again checking that the break point survives.
  // This tests that the function break point was converted to a script break
  // point.
  v8::Script::Compile(script, &origin)->Run();
  g = v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("g")));
  g->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointHitCount);

  // Make sure that the break point numbers are consecutive.
  CHECK_EQ(1, sbp1);
  CHECK_EQ(2, sbp2);
  CHECK_EQ(3, sbp3);
  CHECK_EQ(4, sbp4);
  CHECK_EQ(5, sbp5);
  CHECK_EQ(6, sbp6);
  CHECK_EQ(7, bp7);
}


// Test conditional script break points.
TEST(EnableDisableScriptBreakPoint) {
  break_point_hit_count = 0;
  v8::HandleScope scope;
  DebugLocalContext env;
  env.ExposeDebug();

  v8::Debug::AddDebugEventListener(DebugEventBreakPointHitCount,
                                   v8::Undefined());

  v8::Local<v8::String> script = v8::String::New(
    "function f() {\n"
    "  a = 0;  // line 1\n"
    "};");

  // Compile the script and get function f.
  v8::ScriptOrigin origin =
      v8::ScriptOrigin(v8::String::New("test"));
  v8::Script::Compile(script, &origin)->Run();
  v8::Local<v8::Function> f =
      v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("f")));

  // Set script break point on line 1 (in function f).
  int sbp = SetScriptBreakPointFromJS("test", 1, 0);

  // Call f while enabeling and disabling the script break point.
  break_point_hit_count = 0;
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);

  DisableScriptBreakPointFromJS(sbp);
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);

  EnableScriptBreakPointFromJS(sbp);
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);

  DisableScriptBreakPointFromJS(sbp);
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);

  // Reload the script and get f again checking that the disabeling survives.
  v8::Script::Compile(script, &origin)->Run();
  f = v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("f")));
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);

  EnableScriptBreakPointFromJS(sbp);
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(3, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointHitCount);
}


// Test conditional script break points.
TEST(ConditionalScriptBreakPoint) {
  break_point_hit_count = 0;
  v8::HandleScope scope;
  DebugLocalContext env;
  env.ExposeDebug();

  v8::Debug::AddDebugEventListener(DebugEventBreakPointHitCount,
                                   v8::Undefined());

  v8::Local<v8::String> script = v8::String::New(
    "count = 0;\n"
    "function f() {\n"
    "  g(count++);  // line 2\n"
    "};\n"
    "function g(x) {\n"
    "  var a=x;  // line 5\n"
    "};");

  // Compile the script and get function f.
  v8::ScriptOrigin origin =
      v8::ScriptOrigin(v8::String::New("test"));
  v8::Script::Compile(script, &origin)->Run();
  v8::Local<v8::Function> f =
      v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("f")));

  // Set script break point on line 5 (in function g).
  int sbp1 = SetScriptBreakPointFromJS("test", 5, 0);

  // Call f with different conditions on the script break point.
  break_point_hit_count = 0;
  ChangeScriptBreakPointConditionFromJS(sbp1, "false");
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);

  ChangeScriptBreakPointConditionFromJS(sbp1, "true");
  break_point_hit_count = 0;
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);

  ChangeScriptBreakPointConditionFromJS(sbp1, "a % 2 == 0");
  break_point_hit_count = 0;
  for (int i = 0; i < 10; i++) {
    f->Call(env->Global(), 0, NULL);
  }
  CHECK_EQ(5, break_point_hit_count);

  // Reload the script and get f again checking that the condition survives.
  v8::Script::Compile(script, &origin)->Run();
  f = v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("f")));

  break_point_hit_count = 0;
  for (int i = 0; i < 10; i++) {
    f->Call(env->Global(), 0, NULL);
  }
  CHECK_EQ(5, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointHitCount);
}


// Test ignore count on script break points.
TEST(ScriptBreakPointIgnoreCount) {
  break_point_hit_count = 0;
  v8::HandleScope scope;
  DebugLocalContext env;
  env.ExposeDebug();

  v8::Debug::AddDebugEventListener(DebugEventBreakPointHitCount,
                                   v8::Undefined());

  v8::Local<v8::String> script = v8::String::New(
    "function f() {\n"
    "  a = 0;  // line 1\n"
    "};");

  // Compile the script and get function f.
  v8::ScriptOrigin origin =
      v8::ScriptOrigin(v8::String::New("test"));
  v8::Script::Compile(script, &origin)->Run();
  v8::Local<v8::Function> f =
      v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("f")));

  // Set script break point on line 1 (in function f).
  int sbp = SetScriptBreakPointFromJS("test", 1, 0);

  // Call f with different ignores on the script break point.
  break_point_hit_count = 0;
  ChangeScriptBreakPointIgnoreCountFromJS(sbp, 1);
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);

  ChangeScriptBreakPointIgnoreCountFromJS(sbp, 5);
  break_point_hit_count = 0;
  for (int i = 0; i < 10; i++) {
    f->Call(env->Global(), 0, NULL);
  }
  CHECK_EQ(5, break_point_hit_count);

  // Reload the script and get f again checking that the ignore survives.
  v8::Script::Compile(script, &origin)->Run();
  f = v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("f")));

  break_point_hit_count = 0;
  for (int i = 0; i < 10; i++) {
    f->Call(env->Global(), 0, NULL);
  }
  CHECK_EQ(5, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointHitCount);
}


// Test that script break points survive when a script is reloaded.
TEST(ScriptBreakPointReload) {
  break_point_hit_count = 0;
  v8::HandleScope scope;
  DebugLocalContext env;
  env.ExposeDebug();

  v8::Debug::AddDebugEventListener(DebugEventBreakPointHitCount,
                                   v8::Undefined());

  v8::Local<v8::Function> f;
  v8::Local<v8::String> script = v8::String::New(
    "function f() {\n"
    "  function h() {\n"
    "    a = 0;  // line 2\n"
    "  }\n"
    "  b = 1;  // line 4\n"
    "  return h();\n"
    "}");

  v8::ScriptOrigin origin_1 = v8::ScriptOrigin(v8::String::New("1"));
  v8::ScriptOrigin origin_2 = v8::ScriptOrigin(v8::String::New("2"));

  // Set a script break point before the script is loaded.
  SetScriptBreakPointFromJS("1", 2, 0);

  // Compile the script and get the function.
  v8::Script::Compile(script, &origin_1)->Run();
  f = v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("f")));

  // Call f and check that the script break point is active.
  break_point_hit_count = 0;
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);

  // Compile the script again with a different script data and get the
  // function.
  v8::Script::Compile(script, &origin_2)->Run();
  f = v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("f")));

  // Call f and check that no break points are set.
  break_point_hit_count = 0;
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);

  // Compile the script again and get the function.
  v8::Script::Compile(script, &origin_1)->Run();
  f = v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("f")));

  // Call f and check that the script break point is active.
  break_point_hit_count = 0;
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointHitCount);
}


// Test when several scripts has the same script data
TEST(ScriptBreakPointMultiple) {
  break_point_hit_count = 0;
  v8::HandleScope scope;
  DebugLocalContext env;
  env.ExposeDebug();

  v8::Debug::AddDebugEventListener(DebugEventBreakPointHitCount,
                                   v8::Undefined());

  v8::Local<v8::Function> f;
  v8::Local<v8::String> script_f = v8::String::New(
    "function f() {\n"
    "  a = 0;  // line 1\n"
    "}");

  v8::Local<v8::Function> g;
  v8::Local<v8::String> script_g = v8::String::New(
    "function g() {\n"
    "  b = 0;  // line 1\n"
    "}");

  v8::ScriptOrigin origin =
      v8::ScriptOrigin(v8::String::New("test"));

  // Set a script break point before the scripts are loaded.
  int sbp = SetScriptBreakPointFromJS("test", 1, 0);

  // Compile the scripts with same script data and get the functions.
  v8::Script::Compile(script_f, &origin)->Run();
  f = v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("f")));
  v8::Script::Compile(script_g, &origin)->Run();
  g = v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("g")));

  // Call f and g and check that the script break point is active.
  break_point_hit_count = 0;
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);
  g->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);

  // Clear the script break point.
  ClearBreakPointFromJS(sbp);

  // Call f and g and check that the script break point is no longer active.
  break_point_hit_count = 0;
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);
  g->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);

  // Set script break point with the scripts loaded.
  sbp = SetScriptBreakPointFromJS("test", 1, 0);

  // Call f and g and check that the script break point is active.
  break_point_hit_count = 0;
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);
  g->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointHitCount);
}


// Test the script origin which has both name and line offset.
TEST(ScriptBreakPointLineOffset) {
  break_point_hit_count = 0;
  v8::HandleScope scope;
  DebugLocalContext env;
  env.ExposeDebug();

  v8::Debug::AddDebugEventListener(DebugEventBreakPointHitCount,
                                   v8::Undefined());

  v8::Local<v8::Function> f;
  v8::Local<v8::String> script = v8::String::New(
    "function f() {\n"
    "  a = 0;  // line 8 as this script has line offset 7\n"
    "  b = 0;  // line 9 as this script has line offset 7\n"
    "}");

  // Create script origin both name and line offset.
  v8::ScriptOrigin origin(v8::String::New("test.html"),
                          v8::Integer::New(7));

  // Set two script break points before the script is loaded.
  int sbp1 = SetScriptBreakPointFromJS("test.html", 8, 0);
  int sbp2 = SetScriptBreakPointFromJS("test.html", 9, 0);

  // Compile the script and get the function.
  v8::Script::Compile(script, &origin)->Run();
  f = v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("f")));

  // Call f and check that the script break point is active.
  break_point_hit_count = 0;
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);

  // Clear the script break points.
  ClearBreakPointFromJS(sbp1);
  ClearBreakPointFromJS(sbp2);

  // Call f and check that no script break points are active.
  break_point_hit_count = 0;
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);

  // Set a script break point with the script loaded.
  sbp1 = SetScriptBreakPointFromJS("test.html", 9, 0);

  // Call f and check that the script break point is active.
  break_point_hit_count = 0;
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointHitCount);
}


// Test script break points set on lines.
TEST(ScriptBreakPointLine) {
  v8::HandleScope scope;
  DebugLocalContext env;
  env.ExposeDebug();

  // Create a function for checking the function when hitting a break point.
  frame_function_name = CompileFunction(&env,
                                        frame_function_name_source,
                                        "frame_function_name");

  v8::Debug::AddDebugEventListener(DebugEventBreakPointHitCount,
                                   v8::Undefined());

  v8::Local<v8::Function> f;
  v8::Local<v8::Function> g;
  v8::Local<v8::String> script = v8::String::New(
    "a = 0                      // line 0\n"
    "function f() {\n"
    "  a = 1;                   // line 2\n"
    "}\n"
    " a = 2;                    // line 4\n"
    "  /* xx */ function g() {  // line 5\n"
    "    function h() {         // line 6\n"
    "      a = 3;               // line 7\n"
    "    }\n"
    "    h();                   // line 9\n"
    "    a = 4;                 // line 10\n"
    "  }\n"
    " a=5;                      // line 12");

  // Set a couple script break point before the script is loaded.
  int sbp1 = SetScriptBreakPointFromJS("test.html", 0, -1);
  int sbp2 = SetScriptBreakPointFromJS("test.html", 1, -1);
  int sbp3 = SetScriptBreakPointFromJS("test.html", 5, -1);

  // Compile the script and get the function.
  break_point_hit_count = 0;
  v8::ScriptOrigin origin(v8::String::New("test.html"), v8::Integer::New(0));
  v8::Script::Compile(script, &origin)->Run();
  f = v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("f")));
  g = v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("g")));

  // Chesk that a break point was hit when the script was run.
  CHECK_EQ(1, break_point_hit_count);
  CHECK_EQ(0, strlen(last_function_hit));

  // Call f and check that the script break point.
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);
  CHECK_EQ("f", last_function_hit);

  // Call g and check that the script break point.
  g->Call(env->Global(), 0, NULL);
  CHECK_EQ(3, break_point_hit_count);
  CHECK_EQ("g", last_function_hit);

  // Clear the script break point on g and set one on h.
  ClearBreakPointFromJS(sbp3);
  int sbp4 = SetScriptBreakPointFromJS("test.html", 6, -1);

  // Call g and check that the script break point in h is hit.
  g->Call(env->Global(), 0, NULL);
  CHECK_EQ(4, break_point_hit_count);
  CHECK_EQ("h", last_function_hit);

  // Clear break points in f and h. Set a new one in the script between
  // functions f and g and test that there is no break points in f and g any
  // more.
  ClearBreakPointFromJS(sbp2);
  ClearBreakPointFromJS(sbp4);
  int sbp5 = SetScriptBreakPointFromJS("test.html", 4, -1);
  break_point_hit_count = 0;
  f->Call(env->Global(), 0, NULL);
  g->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);

  // Reload the script which should hit two break points.
  break_point_hit_count = 0;
  v8::Script::Compile(script, &origin)->Run();
  CHECK_EQ(2, break_point_hit_count);
  CHECK_EQ(0, strlen(last_function_hit));

  // Set a break point in the code after the last function decleration.
  int sbp6 = SetScriptBreakPointFromJS("test.html", 12, -1);

  // Reload the script which should hit three break points.
  break_point_hit_count = 0;
  v8::Script::Compile(script, &origin)->Run();
  CHECK_EQ(3, break_point_hit_count);
  CHECK_EQ(0, strlen(last_function_hit));

  // Clear the last break points, and reload the script which should not hit any
  // break points.
  ClearBreakPointFromJS(sbp1);
  ClearBreakPointFromJS(sbp5);
  ClearBreakPointFromJS(sbp6);
  break_point_hit_count = 0;
  v8::Script::Compile(script, &origin)->Run();
  CHECK_EQ(0, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointHitCount);
}


// Test that it is possible to remove the last break point for a function
// inside the break handling of that break point.
TEST(RemoveBreakPointInBreak) {
  v8::HandleScope scope;
  DebugLocalContext env;

  v8::Local<v8::Function> foo =
      CompileFunction(&env, "function foo(){a=1;}", "foo");
  debug_event_remove_break_point = SetBreakPoint(foo, 0);

  // Register the debug event listener pasing the function
  v8::Debug::AddDebugEventListener(DebugEventRemoveBreakPoint, foo);

  break_point_hit_count = 0;
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);

  break_point_hit_count = 0;
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventRemoveBreakPoint);
}


// Test that the debugger statement causes a break.
TEST(DebuggerStatement) {
  break_point_hit_count = 0;
  v8::HandleScope scope;
  DebugLocalContext env;
  v8::Debug::AddDebugEventListener(DebugEventBreakPointHitCount,
                                   v8::Undefined());
  v8::Script::Compile(v8::String::New("function bar(){debugger}"))->Run();
  v8::Script::Compile(v8::String::New(
      "function foo(){debugger;debugger;}"))->Run();
  v8::Local<v8::Function> foo =
      v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("foo")));
  v8::Local<v8::Function> bar =
      v8::Local<v8::Function>::Cast(env->Global()->Get(v8::String::New("bar")));

  // Run function with debugger statement
  bar->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);

  // Run function with two debugger statement
  foo->Call(env->Global(), 0, NULL);
  CHECK_EQ(3, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointHitCount);
}


// Thest that the evaluation of expressions when a break point is hit generates
// the correct results.
TEST(DebugEvaluate) {
  v8::HandleScope scope;
  DebugLocalContext env;
  env.ExposeDebug();

  // Create a function for checking the evaluation when hitting a break point.
  evaluate_check_function = CompileFunction(&env,
                                            evaluate_check_source,
                                            "evaluate_check");
  // Register the debug event listener
  v8::Debug::AddDebugEventListener(DebugEventEvaluate);

  // Different expected vaules of x and a when in a break point (u = undefined,
  // d = Hello, world!).
  struct EvaluateCheck checks_uu[] = {
    {"x", v8::Undefined()},
    {"a", v8::Undefined()},
    {NULL, v8::Handle<v8::Value>()}
  };
  struct EvaluateCheck checks_hu[] = {
    {"x", v8::String::New("Hello, world!")},
    {"a", v8::Undefined()},
    {NULL, v8::Handle<v8::Value>()}
  };
  struct EvaluateCheck checks_hh[] = {
    {"x", v8::String::New("Hello, world!")},
    {"a", v8::String::New("Hello, world!")},
    {NULL, v8::Handle<v8::Value>()}
  };

  // Simple test function. The "y=0" is in the function foo to provide a break
  // location. For "y=0" the "y" is at position 15 in the barbar function
  // therefore setting breakpoint at position 15 will break at "y=0" and
  // setting it higher will break after.
  v8::Local<v8::Function> foo = CompileFunction(&env,
    "function foo(x) {"
    "  var a;"
    "  y=0; /* To ensure break location.*/"
    "  a=x;"
    "}",
    "foo");
  const int foo_break_position = 15;

  // Arguments with one parameter "Hello, world!"
  v8::Handle<v8::Value> argv_foo[1] = { v8::String::New("Hello, world!") };

  // Call foo with breakpoint set before a=x and undefined as parameter.
  int bp = SetBreakPoint(foo, foo_break_position);
  checks = checks_uu;
  foo->Call(env->Global(), 0, NULL);

  // Call foo with breakpoint set before a=x and parameter "Hello, world!".
  checks = checks_hu;
  foo->Call(env->Global(), 1, argv_foo);

  // Call foo with breakpoint set after a=x and parameter "Hello, world!".
  ClearBreakPoint(bp);
  SetBreakPoint(foo, foo_break_position + 1);
  checks = checks_hh;
  foo->Call(env->Global(), 1, argv_foo);

  // Test function with an inner function. The "y=0" is in function barbar
  // to provide a break location. For "y=0" the "y" is at position 8 in the
  // barbar function therefore setting breakpoint at position 8 will break at
  // "y=0" and setting it higher will break after.
  v8::Local<v8::Function> bar = CompileFunction(&env,
    "y = 0;"
    "x = 'Goodbye, world!';"
    "function bar(x, b) {"
    "  var a;"
    "  function barbar() {"
    "    y=0; /* To ensure break location.*/"
    "    a=x;"
    "  };"
    "  debug.Debug.clearAllBreakPoints();"
    "  barbar();"
    "  y=0;a=x;"
    "}",
    "bar");
  const int barbar_break_position = 8;

  // Call bar setting breakpoint before a=x in barbar and undefined as
  // parameter.
  checks = checks_uu;
  v8::Handle<v8::Value> argv_bar_1[2] = {
    v8::Undefined(),
    v8::Number::New(barbar_break_position)
  };
  bar->Call(env->Global(), 2, argv_bar_1);

  // Call bar setting breakpoint before a=x in barbar and parameter
  // "Hello, world!".
  checks = checks_hu;
  v8::Handle<v8::Value> argv_bar_2[2] = {
    v8::String::New("Hello, world!"),
    v8::Number::New(barbar_break_position)
  };
  bar->Call(env->Global(), 2, argv_bar_2);

  // Call bar setting breakpoint after a=x in barbar and parameter
  // "Hello, world!".
  checks = checks_hh;
  v8::Handle<v8::Value> argv_bar_3[2] = {
    v8::String::New("Hello, world!"),
    v8::Number::New(barbar_break_position + 1)
  };
  bar->Call(env->Global(), 2, argv_bar_3);

  v8::Debug::RemoveDebugEventListener(DebugEventEvaluate);
}


// Simple test of the stepping mechanism using only store ICs.
TEST(DebugStepLinear) {
  v8::HandleScope scope;
  DebugLocalContext env;

  // Create a function for testing stepping.
  v8::Local<v8::Function> foo = CompileFunction(&env,
                                                "function foo(){a=1;b=1;c=1;}",
                                                "foo");
  SetBreakPoint(foo, 3);

  // Register a debug event listener which steps and counts.
  v8::Debug::AddDebugEventListener(DebugEventStep);

  step_action = StepIn;
  break_point_hit_count = 0;
  foo->Call(env->Global(), 0, NULL);

  // With stepping all break locations are hit.
  CHECK_EQ(4, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventStep);

  // Register a debug event listener which just counts.
  v8::Debug::AddDebugEventListener(DebugEventBreakPointHitCount);

  break_point_hit_count = 0;
  foo->Call(env->Global(), 0, NULL);

  // Without stepping only active break points are hit.
  CHECK_EQ(1, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointHitCount);
}


// Test the stepping mechanism with different ICs.
TEST(DebugStepLinearMixedICs) {
  v8::HandleScope scope;
  DebugLocalContext env;

  // Create a function for testing stepping.
  v8::Local<v8::Function> foo = CompileFunction(&env,
      "function bar() {};"
      "function foo() {"
      "  var x;"
      "  var index='name';"
      "  var y = {};"
      "  a=1;b=2;x=a;y[index]=3;x=y[index];bar();}", "foo");
  SetBreakPoint(foo, 0);

  // Register a debug event listener which steps and counts.
  v8::Debug::AddDebugEventListener(DebugEventStep);

  step_action = StepIn;
  break_point_hit_count = 0;
  foo->Call(env->Global(), 0, NULL);

  // With stepping all break locations are hit. For ARM the keyed load/store
  // is not hit as they are not implemented as ICs.
#if defined (__arm__) || defined(__thumb__)
  CHECK_EQ(6, break_point_hit_count);
#else
  CHECK_EQ(8, break_point_hit_count);
#endif

  v8::Debug::RemoveDebugEventListener(DebugEventStep);

  // Register a debug event listener which just counts.
  v8::Debug::AddDebugEventListener(DebugEventBreakPointHitCount);

  break_point_hit_count = 0;
  foo->Call(env->Global(), 0, NULL);

  // Without stepping only active break points are hit.
  CHECK_EQ(1, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointHitCount);
}


TEST(DebugStepIf) {
  v8::HandleScope scope;
  DebugLocalContext env;

  // Register a debug event listener which steps and counts.
  v8::Debug::AddDebugEventListener(DebugEventStep);

  // Create a function for testing stepping.
  const int argc = 1;
  const char* src = "function foo(x) { "
                    "  a = 1;"
                    "  if (x) {"
                    "    b = 1;"
                    "  } else {"
                    "    c = 1;"
                    "    d = 1;"
                    "  }"
                    "}";
  v8::Local<v8::Function> foo = CompileFunction(&env, src, "foo");
  SetBreakPoint(foo, 0);

  // Stepping through the true part.
  step_action = StepIn;
  break_point_hit_count = 0;
  v8::Handle<v8::Value> argv_true[argc] = { v8::True() };
  foo->Call(env->Global(), argc, argv_true);
  CHECK_EQ(3, break_point_hit_count);

  // Stepping through the false part.
  step_action = StepIn;
  break_point_hit_count = 0;
  v8::Handle<v8::Value> argv_false[argc] = { v8::False() };
  foo->Call(env->Global(), argc, argv_false);
  CHECK_EQ(4, break_point_hit_count);

  // Get rid of the debug event listener.
  v8::Debug::RemoveDebugEventListener(DebugEventStep);
}


TEST(DebugStepSwitch) {
  v8::HandleScope scope;
  DebugLocalContext env;

  // Register a debug event listener which steps and counts.
  v8::Debug::AddDebugEventListener(DebugEventStep);

  // Create a function for testing stepping.
  const int argc = 1;
  const char* src = "function foo(x) { "
                    "  a = 1;"
                    "  switch (x) {"
                    "    case 1:"
                    "      b = 1;"
                    "    case 2:"
                    "      c = 1;"
                    "      break;"
                    "    case 3:"
                    "      d = 1;"
                    "      e = 1;"
                    "      break;"
                    "  }"
                    "}";
  v8::Local<v8::Function> foo = CompileFunction(&env, src, "foo");
  SetBreakPoint(foo, 0);

  // One case with fall-through.
  step_action = StepIn;
  break_point_hit_count = 0;
  v8::Handle<v8::Value> argv_1[argc] = { v8::Number::New(1) };
  foo->Call(env->Global(), argc, argv_1);
  CHECK_EQ(4, break_point_hit_count);

  // Another case.
  step_action = StepIn;
  break_point_hit_count = 0;
  v8::Handle<v8::Value> argv_2[argc] = { v8::Number::New(2) };
  foo->Call(env->Global(), argc, argv_2);
  CHECK_EQ(3, break_point_hit_count);

  // Last case.
  step_action = StepIn;
  break_point_hit_count = 0;
  v8::Handle<v8::Value> argv_3[argc] = { v8::Number::New(3) };
  foo->Call(env->Global(), argc, argv_3);
  CHECK_EQ(4, break_point_hit_count);

  // Get rid of the debug event listener.
  v8::Debug::RemoveDebugEventListener(DebugEventStep);
}


TEST(DebugStepFor) {
  v8::HandleScope scope;
  DebugLocalContext env;

  // Register a debug event listener which steps and counts.
  v8::Debug::AddDebugEventListener(DebugEventStep);

  // Create a function for testing stepping.
  const int argc = 1;
  const char* src = "function foo(x) { "
                    "  a = 1;"
                    "  for (i = 0; i < x; i++) {"
                    "    b = 1;"
                    "  }"
                    "}";
  v8::Local<v8::Function> foo = CompileFunction(&env, src, "foo");
  SetBreakPoint(foo, 8);  // "a = 1;"

  // Looping 10 times.
  step_action = StepIn;
  break_point_hit_count = 0;
  v8::Handle<v8::Value> argv_10[argc] = { v8::Number::New(10) };
  foo->Call(env->Global(), argc, argv_10);
  CHECK_EQ(23, break_point_hit_count);

  // Looping 100 times.
  step_action = StepIn;
  break_point_hit_count = 0;
  v8::Handle<v8::Value> argv_100[argc] = { v8::Number::New(100) };
  foo->Call(env->Global(), argc, argv_100);
  CHECK_EQ(203, break_point_hit_count);

  // Get rid of the debug event listener.
  v8::Debug::RemoveDebugEventListener(DebugEventStep);
}


TEST(StepInOutSimple) {
  v8::HandleScope scope;
  DebugLocalContext env;

  // Create a function for checking the function when hitting a break point.
  frame_function_name = CompileFunction(&env,
                                        frame_function_name_source,
                                        "frame_function_name");

  // Register a debug event listener which steps and counts.
  v8::Debug::AddDebugEventListener(DebugEventStepSequence);

  // Create functions for testing stepping.
  const char* src = "function a() {b();c();}; "
                    "function b() {c();}; "
                    "function c() {}; ";
  v8::Local<v8::Function> a = CompileFunction(&env, src, "a");
  SetBreakPoint(a, 0);

  // Step through invocation of a with step in.
  step_action = StepIn;
  break_point_hit_count = 0;
  expected_step_sequence = "abcbaca";
  a->Call(env->Global(), 0, NULL);
  CHECK_EQ(strlen(expected_step_sequence), break_point_hit_count);

  // Step through invocation of a with step next.
  step_action = StepNext;
  break_point_hit_count = 0;
  expected_step_sequence = "aaa";
  a->Call(env->Global(), 0, NULL);
  CHECK_EQ(strlen(expected_step_sequence), break_point_hit_count);

  // Step through invocation of a with step out.
  step_action = StepOut;
  break_point_hit_count = 0;
  expected_step_sequence = "a";
  a->Call(env->Global(), 0, NULL);
  CHECK_EQ(strlen(expected_step_sequence), break_point_hit_count);

  // Get rid of the debug event listener.
  v8::Debug::RemoveDebugEventListener(DebugEventStepSequence);
}


TEST(StepInOutTree) {
  v8::HandleScope scope;
  DebugLocalContext env;

  // Create a function for checking the function when hitting a break point.
  frame_function_name = CompileFunction(&env,
                                        frame_function_name_source,
                                        "frame_function_name");

  // Register a debug event listener which steps and counts.
  v8::Debug::AddDebugEventListener(DebugEventStepSequence);

  // Create functions for testing stepping.
  const char* src = "function a() {b(c(d()),d());c(d());d()}; "
                    "function b(x,y) {c();}; "
                    "function c(x) {}; "
                    "function d() {}; ";
  v8::Local<v8::Function> a = CompileFunction(&env, src, "a");
  SetBreakPoint(a, 0);

  // Step through invocation of a with step in.
  step_action = StepIn;
  break_point_hit_count = 0;
  expected_step_sequence = "adacadabcbadacada";
  a->Call(env->Global(), 0, NULL);
  CHECK_EQ(strlen(expected_step_sequence), break_point_hit_count);

  // Step through invocation of a with step next.
  step_action = StepNext;
  break_point_hit_count = 0;
  expected_step_sequence = "aaaa";
  a->Call(env->Global(), 0, NULL);
  CHECK_EQ(strlen(expected_step_sequence), break_point_hit_count);

  // Step through invocation of a with step out.
  step_action = StepOut;
  break_point_hit_count = 0;
  expected_step_sequence = "a";
  a->Call(env->Global(), 0, NULL);
  CHECK_EQ(strlen(expected_step_sequence), break_point_hit_count);

  // Get rid of the debug event listener.
  v8::Debug::RemoveDebugEventListener(DebugEventStepSequence);
}


TEST(StepInOutBranch) {
  v8::HandleScope scope;
  DebugLocalContext env;

  // Create a function for checking the function when hitting a break point.
  frame_function_name = CompileFunction(&env,
                                        frame_function_name_source,
                                        "frame_function_name");

  // Register a debug event listener which steps and counts.
  v8::Debug::AddDebugEventListener(DebugEventStepSequence);

  // Create functions for testing stepping.
  const char* src = "function a() {b(false);c();}; "
                    "function b(x) {if(x){c();};}; "
                    "function c() {}; ";
  v8::Local<v8::Function> a = CompileFunction(&env, src, "a");
  SetBreakPoint(a, 0);

  // Step through invocation of a.
  step_action = StepIn;
  break_point_hit_count = 0;
  expected_step_sequence = "abaca";
  a->Call(env->Global(), 0, NULL);
  CHECK_EQ(strlen(expected_step_sequence), break_point_hit_count);

  // Get rid of the debug event listener.
  v8::Debug::RemoveDebugEventListener(DebugEventStepSequence);
}


// Test that step in does not step into native functions.
TEST(DebugStepNatives) {
  v8::HandleScope scope;
  DebugLocalContext env;

  // Create a function for testing stepping.
  v8::Local<v8::Function> foo = CompileFunction(
      &env,
      "function foo(){debugger;Math.sin(1);}",
      "foo");

  // Register a debug event listener which steps and counts.
  v8::Debug::AddDebugEventListener(DebugEventStep);

  step_action = StepIn;
  break_point_hit_count = 0;
  foo->Call(env->Global(), 0, NULL);

  // With stepping all break locations are hit.
  CHECK_EQ(3, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventStep);

  // Register a debug event listener which just counts.
  v8::Debug::AddDebugEventListener(DebugEventBreakPointHitCount);

  break_point_hit_count = 0;
  foo->Call(env->Global(), 0, NULL);

  // Without stepping only active break points are hit.
  CHECK_EQ(1, break_point_hit_count);

  v8::Debug::RemoveDebugEventListener(DebugEventBreakPointHitCount);
}


// Test break on exceptions. For each exception break combination the number
// of debug event exception callbacks and message callbacks are collected. The
// number of debug event exception callbacks are cused to check that the
// debugger is called correctly and the number of message callbacks is used to
// check that uncaught exceptions are still returned even if there is a break
// for them.
TEST(BreakOnException) {
  v8::HandleScope scope;
  DebugLocalContext env;
  env.ExposeDebug();

  v8::internal::Top::TraceException(false);

  // Create functions for testing break on exception.
  v8::Local<v8::Function> throws =
      CompileFunction(&env, "function throws(){throw 1;}", "throws");
  v8::Local<v8::Function> caught =
      CompileFunction(&env,
                      "function caught(){try {throws();} catch(e) {};}",
                      "caught");
  v8::Local<v8::Function> notCaught =
      CompileFunction(&env, "function notCaught(){throws();}", "notCaught");

  v8::V8::AddMessageListener(MessageCallbackCount);
  v8::Debug::AddDebugEventListener(DebugEventCounter);

  // Initial state should be break on uncaught exception.
  DebugEventCounterClear();
  MessageCallbackCountClear();
  caught->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, exception_hit_count);
  CHECK_EQ(0, uncaught_exception_hit_count);
  CHECK_EQ(0, message_callback_count);
  notCaught->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, exception_hit_count);
  CHECK_EQ(1, uncaught_exception_hit_count);
  CHECK_EQ(1, message_callback_count);

  // No break on exception
  DebugEventCounterClear();
  MessageCallbackCountClear();
  ChangeBreakOnException(false, false);
  caught->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, exception_hit_count);
  CHECK_EQ(0, uncaught_exception_hit_count);
  CHECK_EQ(0, message_callback_count);
  notCaught->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, exception_hit_count);
  CHECK_EQ(0, uncaught_exception_hit_count);
  CHECK_EQ(1, message_callback_count);

  // Break on uncaught exception
  DebugEventCounterClear();
  MessageCallbackCountClear();
  ChangeBreakOnException(false, true);
  caught->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, exception_hit_count);
  CHECK_EQ(0, uncaught_exception_hit_count);
  CHECK_EQ(0, message_callback_count);
  notCaught->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, exception_hit_count);
  CHECK_EQ(1, uncaught_exception_hit_count);
  CHECK_EQ(1, message_callback_count);

  // Break on exception and uncaught exception
  DebugEventCounterClear();
  MessageCallbackCountClear();
  ChangeBreakOnException(true, true);
  caught->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, exception_hit_count);
  CHECK_EQ(0, uncaught_exception_hit_count);
  CHECK_EQ(0, message_callback_count);
  notCaught->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, exception_hit_count);
  CHECK_EQ(1, uncaught_exception_hit_count);
  CHECK_EQ(1, message_callback_count);

  // Break on exception
  DebugEventCounterClear();
  MessageCallbackCountClear();
  ChangeBreakOnException(true, false);
  caught->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, exception_hit_count);
  CHECK_EQ(0, uncaught_exception_hit_count);
  CHECK_EQ(0, message_callback_count);
  notCaught->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, exception_hit_count);
  CHECK_EQ(1, uncaught_exception_hit_count);
  CHECK_EQ(1, message_callback_count);

  // No break on exception using JavaScript
  DebugEventCounterClear();
  MessageCallbackCountClear();
  ChangeBreakOnExceptionFromJS(false, false);
  caught->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, exception_hit_count);
  CHECK_EQ(0, uncaught_exception_hit_count);
  CHECK_EQ(0, message_callback_count);
  notCaught->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, exception_hit_count);
  CHECK_EQ(0, uncaught_exception_hit_count);
  CHECK_EQ(1, message_callback_count);

  // Break on uncaught exception using JavaScript
  DebugEventCounterClear();
  MessageCallbackCountClear();
  ChangeBreakOnExceptionFromJS(false, true);
  caught->Call(env->Global(), 0, NULL);
  CHECK_EQ(0, exception_hit_count);
  CHECK_EQ(0, uncaught_exception_hit_count);
  CHECK_EQ(0, message_callback_count);
  notCaught->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, exception_hit_count);
  CHECK_EQ(1, uncaught_exception_hit_count);
  CHECK_EQ(1, message_callback_count);

  // Break on exception and uncaught exception using JavaScript
  DebugEventCounterClear();
  MessageCallbackCountClear();
  ChangeBreakOnExceptionFromJS(true, true);
  caught->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, exception_hit_count);
  CHECK_EQ(0, message_callback_count);
  CHECK_EQ(0, uncaught_exception_hit_count);
  notCaught->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, exception_hit_count);
  CHECK_EQ(1, uncaught_exception_hit_count);
  CHECK_EQ(1, message_callback_count);

  // Break on exception using JavaScript
  DebugEventCounterClear();
  MessageCallbackCountClear();
  ChangeBreakOnExceptionFromJS(true, false);
  caught->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, exception_hit_count);
  CHECK_EQ(0, uncaught_exception_hit_count);
  CHECK_EQ(0, message_callback_count);
  notCaught->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, exception_hit_count);
  CHECK_EQ(1, uncaught_exception_hit_count);
  CHECK_EQ(1, message_callback_count);

  v8::Debug::RemoveDebugEventListener(DebugEventCounter);
  v8::V8::RemoveMessageListeners(MessageCallbackCount);
}


TEST(StepWithException) {
  v8::HandleScope scope;
  DebugLocalContext env;

  // Create a function for checking the function when hitting a break point.
  frame_function_name = CompileFunction(&env,
                                        frame_function_name_source,
                                        "frame_function_name");

  // Register a debug event listener which steps and counts.
  v8::Debug::AddDebugEventListener(DebugEventStepSequence);

  // Create functions for testing stepping.
  const char* src = "function a() { n(); }; "
                    "function b() { c(); }; "
                    "function c() { n(); }; "
                    "function d() { x = 1; try { e(); } catch(x) { x = 2; } }; "
                    "function e() { n(); }; "
                    "function f() { x = 1; try { g(); } catch(x) { x = 2; } }; "
                    "function g() { h(); }; "
                    "function h() { x = 1; throw 1; }; ";

  // Step through invocation of a.
  v8::Local<v8::Function> a = CompileFunction(&env, src, "a");
  SetBreakPoint(a, 0);
  step_action = StepIn;
  break_point_hit_count = 0;
  expected_step_sequence = "aa";
  a->Call(env->Global(), 0, NULL);
  CHECK_EQ(strlen(expected_step_sequence), break_point_hit_count);

  // Step through invocation of b + c.
  v8::Local<v8::Function> b = CompileFunction(&env, src, "b");
  SetBreakPoint(b, 0);
  step_action = StepIn;
  break_point_hit_count = 0;
  expected_step_sequence = "bcc";
  b->Call(env->Global(), 0, NULL);
  CHECK_EQ(strlen(expected_step_sequence), break_point_hit_count);

  // Step through invocation of d + e.
  v8::Local<v8::Function> d = CompileFunction(&env, src, "d");
  SetBreakPoint(d, 0);
  ChangeBreakOnException(false, true);
  step_action = StepIn;
  break_point_hit_count = 0;
  expected_step_sequence = "ddedd";
  d->Call(env->Global(), 0, NULL);
  CHECK_EQ(strlen(expected_step_sequence), break_point_hit_count);

  // Step through invocation of d + e now with break on caught exceptions.
  ChangeBreakOnException(true, true);
  step_action = StepIn;
  break_point_hit_count = 0;
  expected_step_sequence = "ddeedd";
  d->Call(env->Global(), 0, NULL);
  CHECK_EQ(strlen(expected_step_sequence), break_point_hit_count);

  // Step through invocation of f + g + h.
  v8::Local<v8::Function> f = CompileFunction(&env, src, "f");
  SetBreakPoint(f, 0);
  ChangeBreakOnException(false, true);
  step_action = StepIn;
  break_point_hit_count = 0;
  expected_step_sequence = "ffghff";
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(strlen(expected_step_sequence), break_point_hit_count);

  // Step through invocation of f + g + h now with break on caught exceptions.
  ChangeBreakOnException(true, true);
  step_action = StepIn;
  break_point_hit_count = 0;
  expected_step_sequence = "ffghhff";
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(strlen(expected_step_sequence), break_point_hit_count);

  // Get rid of the debug event listener.
  v8::Debug::RemoveDebugEventListener(DebugEventStepSequence);
}


TEST(DebugBreak) {
  v8::HandleScope scope;
  DebugLocalContext env;

  // This test should be run with option --verify-heap. This is an ASSERT and
  // not a CHECK as --verify-heap is only available in debug mode.
  ASSERT(v8::internal::FLAG_verify_heap);

  // Register a debug event listener which sets the break flag and counts.
  v8::Debug::AddDebugEventListener(DebugEventBreak);

  // Create a function for testing stepping.
  const char* src = "function f0() {}"
                    "function f1(x1) {}"
                    "function f2(x1,x2) {}"
                    "function f3(x1,x2,x3) {}";
  v8::Local<v8::Function> f0 = CompileFunction(&env, src, "f0");
  v8::Local<v8::Function> f1 = CompileFunction(&env, src, "f1");
  v8::Local<v8::Function> f2 = CompileFunction(&env, src, "f2");
  v8::Local<v8::Function> f3 = CompileFunction(&env, src, "f3");

  // Call the function to make sure it is compiled.
  v8::Handle<v8::Value> argv[] = { v8::Number::New(1),
                                   v8::Number::New(1),
                                   v8::Number::New(1),
                                   v8::Number::New(1) };

  // Call all functions to make sure that they are compiled.
  f0->Call(env->Global(), 0, NULL);
  f1->Call(env->Global(), 0, NULL);
  f2->Call(env->Global(), 0, NULL);
  f3->Call(env->Global(), 0, NULL);

  // Set the debug break flag.
  v8::Debug::DebugBreak();

  // Call all functions with different argument count.
  break_point_hit_count = 0;
  for (unsigned int i = 0; i < ARRAY_SIZE(argv); i++) {
    f0->Call(env->Global(), i, argv);
    f1->Call(env->Global(), i, argv);
    f2->Call(env->Global(), i, argv);
    f3->Call(env->Global(), i, argv);
  }

  // One break for each function called.
  CHECK_EQ(4 * ARRAY_SIZE(argv), break_point_hit_count);

  // Get rid of the debug event listener.
  v8::Debug::RemoveDebugEventListener(DebugEventBreak);
}


// Test to ensure that JavaScript code keeps running while the debug break
// through the stack limit flag is set but breaks are disabled.
TEST(DisableBreak) {
  v8::HandleScope scope;
  DebugLocalContext env;

  // Register a debug event listener which sets the break flag and counts.
  v8::Debug::AddDebugEventListener(DebugEventCounter);

  // Create a function for testing stepping.
  const char* src = "function f() {g()};function g(){i=0; while(i<10){i++}}";
  v8::Local<v8::Function> f = CompileFunction(&env, src, "f");

  // Set the debug break flag.
  v8::Debug::DebugBreak();

  // Call all functions with different argument count.
  break_point_hit_count = 0;
  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(1, break_point_hit_count);

  {
    v8::Debug::DebugBreak();
    v8::internal::DisableBreak disable_break(true);
    f->Call(env->Global(), 0, NULL);
    CHECK_EQ(1, break_point_hit_count);
  }

  f->Call(env->Global(), 0, NULL);
  CHECK_EQ(2, break_point_hit_count);

  // Get rid of the debug event listener.
  v8::Debug::RemoveDebugEventListener(DebugEventBreak);
}


static v8::Handle<v8::Array> NamedEnum(const v8::AccessorInfo&) {
  v8::Handle<v8::Array> result = v8::Array::New(3);
  result->Set(v8::Integer::New(0), v8::String::New("a"));
  result->Set(v8::Integer::New(1), v8::String::New("b"));
  result->Set(v8::Integer::New(2), v8::String::New("c"));
  return result;
}


static v8::Handle<v8::Array> IndexedEnum(const v8::AccessorInfo&) {
  v8::Handle<v8::Array> result = v8::Array::New(2);
  result->Set(v8::Integer::New(0), v8::Number::New(1));
  result->Set(v8::Integer::New(1), v8::Number::New(10));
  return result;
}


static v8::Handle<v8::Value> NamedGetter(v8::Local<v8::String> name,
                                         const v8::AccessorInfo& info) {
  v8::String::AsciiValue n(name);
  if (strcmp(*n, "a") == 0) {
    return v8::String::New("AA");
  } else if (strcmp(*n, "b") == 0) {
    return v8::String::New("BB");
  } else if (strcmp(*n, "c") == 0) {
    return v8::String::New("CC");
  } else {
    return v8::Undefined();
  }

  return name;
}


static v8::Handle<v8::Value> IndexedGetter(uint32_t index,
                                           const v8::AccessorInfo& info) {
  return v8::Number::New(index + 1);
}


TEST(InterceptorPropertyMirror) {
  // Create a V8 environment with debug access.
  v8::HandleScope scope;
  DebugLocalContext env;
  env.ExposeDebug();

  // Create object with named interceptor.
  v8::Handle<v8::ObjectTemplate> named = v8::ObjectTemplate::New();
  named->SetNamedPropertyHandler(NamedGetter, NULL, NULL, NULL, NamedEnum);
  env->Global()->Set(v8::String::New("intercepted_named"),
                     named->NewInstance());

  // Create object with indexed interceptor.
  v8::Handle<v8::ObjectTemplate> indexed = v8::ObjectTemplate::New();
  indexed->SetIndexedPropertyHandler(IndexedGetter,
                                     NULL,
                                     NULL,
                                     NULL,
                                     IndexedEnum);
  env->Global()->Set(v8::String::New("intercepted_indexed"),
                     indexed->NewInstance());

  // Create object with both named and indexed interceptor.
  v8::Handle<v8::ObjectTemplate> both = v8::ObjectTemplate::New();
  both->SetNamedPropertyHandler(NamedGetter, NULL, NULL, NULL, NamedEnum);
  both->SetIndexedPropertyHandler(IndexedGetter, NULL, NULL, NULL, IndexedEnum);
  env->Global()->Set(v8::String::New("intercepted_both"), both->NewInstance());

  // Get mirrors for the three objects with interceptor.
  CompileRun(
      "named_mirror = debug.MakeMirror(intercepted_named);"
      "indexed_mirror = debug.MakeMirror(intercepted_indexed);"
      "both_mirror = debug.MakeMirror(intercepted_both)");
  CHECK(CompileRun(
       "named_mirror instanceof debug.ObjectMirror")->BooleanValue());
  CHECK(CompileRun(
        "indexed_mirror instanceof debug.ObjectMirror")->BooleanValue());
  CHECK(CompileRun(
        "both_mirror instanceof debug.ObjectMirror")->BooleanValue());

  // Get the property names from the interceptors
  CompileRun(
      "named_names = named_mirror.interceptorPropertyNames();"
      "indexed_names = indexed_mirror.interceptorPropertyNames();"
      "both_names = both_mirror.interceptorPropertyNames()");
  CHECK_EQ(3, CompileRun("named_names.length")->Int32Value());
  CHECK_EQ(2, CompileRun("indexed_names.length")->Int32Value());
  CHECK_EQ(5, CompileRun("both_names.length")->Int32Value());

  // Check the expected number of properties.
  const char* source;
  source = "named_mirror.interceptorProperties().length";
  CHECK_EQ(3, CompileRun(source)->Int32Value());

  source = "indexed_mirror.interceptorProperties().length";
  CHECK_EQ(2, CompileRun(source)->Int32Value());

  source = "both_mirror.interceptorProperties().length";
  CHECK_EQ(5, CompileRun(source)->Int32Value());

  source = "both_mirror.interceptorProperties(1).length";
  CHECK_EQ(3, CompileRun(source)->Int32Value());

  source = "both_mirror.interceptorProperties(2).length";
  CHECK_EQ(2, CompileRun(source)->Int32Value());

  source = "both_mirror.interceptorProperties(3).length";
  CHECK_EQ(5, CompileRun(source)->Int32Value());

  // Get the interceptor properties for the object with both types of
  // interceptors.
  CompileRun("both_values = both_mirror.interceptorProperties()");

  // Check the mirror hierachy
  source = "both_values[0] instanceof debug.PropertyMirror";
  CHECK(CompileRun(source)->BooleanValue());

  source = "both_values[0] instanceof debug.InterceptorPropertyMirror";
  CHECK(CompileRun(source)->BooleanValue());

  source = "both_values[1] instanceof debug.InterceptorPropertyMirror";
  CHECK(CompileRun(source)->BooleanValue());

  source = "both_values[2] instanceof debug.InterceptorPropertyMirror";
  CHECK(CompileRun(source)->BooleanValue());

  source = "both_values[3] instanceof debug.PropertyMirror";
  CHECK(CompileRun(source)->BooleanValue());

  source = "both_values[3] instanceof debug.InterceptorPropertyMirror";
  CHECK(CompileRun(source)->BooleanValue());

  source = "both_values[4] instanceof debug.InterceptorPropertyMirror";
  CHECK(CompileRun(source)->BooleanValue());

  // Check the property names.
  source = "both_values[0].name() == 'a'";
  CHECK(CompileRun(source)->BooleanValue());

  source = "both_values[1].name() == 'b'";
  CHECK(CompileRun(source)->BooleanValue());

  source = "both_values[2].name() == 'c'";
  CHECK(CompileRun(source)->BooleanValue());

  source = "both_values[3].name() == 1";
  CHECK(CompileRun(source)->BooleanValue());

  source = "both_values[4].name() == 10";
  CHECK(CompileRun(source)->BooleanValue());

  // Check the property values.
  source = "both_values[0].value().value() == 'AA'";
  CHECK(CompileRun(source)->BooleanValue());

  source = "both_values[1].value().value() == 'BB'";
  CHECK(CompileRun(source)->BooleanValue());

  source = "both_values[2].value().value() == 'CC'";
  CHECK(CompileRun(source)->BooleanValue());

  source = "both_values[3].value().value() == 2";
  CHECK(CompileRun(source)->BooleanValue());

  source = "both_values[4].value().value() == 11";
  CHECK(CompileRun(source)->BooleanValue());
}


// Multithreaded tests of JSON debugger protocol

// Support classes

// Copies a C string to a 16-bit string.  Does not check for buffer overflow.
// Does not use the V8 engine to convert strings, so it can be used
// in any thread.  Returns the length of the string.
int AsciiToUtf16(const char* input_buffer, uint16_t* output_buffer) {
  int i;
  for (i = 0; input_buffer[i] != '\0'; ++i) {
    // ASCII does not use chars > 127, but be careful anyway.
    output_buffer[i] = static_cast<unsigned char>(input_buffer[i]);
  }
  output_buffer[i] = 0;
  return i;
}

// Copies a 16-bit string to a C string by dropping the high byte of
// each character.  Does not check for buffer overflow.
// Can be used in any thread.  Requires string length as an input.
int Utf16ToAscii(const uint16_t* input_buffer, int length,
                 char* output_buffer) {
  for (int i = 0; i < length; ++i) {
    output_buffer[i] = static_cast<char>(input_buffer[i]);
  }
  output_buffer[length] = '\0';
  return length;
}

// Provides synchronization between k threads, where k is an input to the
// constructor.  The Wait() call blocks a thread until it is called for the
// k'th time, then all calls return.  Each ThreadBarrier object can only
// be used once.
class ThreadBarrier {
 public:
  explicit ThreadBarrier(int num_threads);
  ~ThreadBarrier();
  void Wait();
 private:
  int num_threads_;
  int num_blocked_;
  v8::internal::Mutex* lock_;
  v8::internal::Semaphore* sem_;
  bool invalid_;
};

ThreadBarrier::ThreadBarrier(int num_threads)
    : num_threads_(num_threads), num_blocked_(0) {
  lock_ = OS::CreateMutex();
  sem_ = OS::CreateSemaphore(0);
  invalid_ = false;  // A barrier may only be used once.  Then it is invalid.
}

// Do not call, due to race condition with Wait().
// Could be resolved with Pthread condition variables.
ThreadBarrier::~ThreadBarrier() {
  lock_->Lock();
  delete lock_;
  delete sem_;
}

void ThreadBarrier::Wait() {
  lock_->Lock();
  ASSERT(!invalid_);
  if (num_blocked_ == num_threads_ - 1) {
    // Signal and unblock all waiting threads.
    for (int i = 0; i < num_threads_ - 1; ++i) {
      sem_->Signal();
    }
    invalid_ = true;
    printf("BARRIER\n\n");
    fflush(stdout);
    lock_->Unlock();
  } else {  // Wait for the semaphore.
    ++num_blocked_;
    lock_->Unlock();  // Potential race condition with destructor because
    sem_->Wait();  // these two lines are not atomic.
  }
}

// A set containing enough barriers and semaphores for any of the tests.
class Barriers {
 public:
  Barriers();
  void Initialize();
  ThreadBarrier barrier_1;
  ThreadBarrier barrier_2;
  ThreadBarrier barrier_3;
  ThreadBarrier barrier_4;
  ThreadBarrier barrier_5;
  v8::internal::Semaphore* semaphore_1;
  v8::internal::Semaphore* semaphore_2;
};

Barriers::Barriers() : barrier_1(2), barrier_2(2),
    barrier_3(2), barrier_4(2), barrier_5(2) {}

void Barriers::Initialize() {
  semaphore_1 = OS::CreateSemaphore(0);
  semaphore_2 = OS::CreateSemaphore(0);
}


// We match parts of the message to decide if it is a break message.
bool IsBreakEventMessage(char *message) {
  const char* type_event = "\"type\":\"event\"";
  const char* event_break = "\"event\":\"break\"";
  // Does the message contain both type:event and event:break?
  return strstr(message, type_event) != NULL &&
         strstr(message, event_break) != NULL;
}


/* Test MessageQueues */
/* Tests the message queues that hold debugger commands and
 * response messages to the debugger.  Fills queues and makes
 * them grow.
 */
Barriers message_queue_barriers;

// This is the debugger thread, that executes no v8 calls except
// placing JSON debugger commands in the queue.
class MessageQueueDebuggerThread : public v8::internal::Thread {
 public:
  void Run();
};

static void MessageHandler(const uint16_t* message, int length, void *data) {
  static char print_buffer[1000];
  Utf16ToAscii(message, length, print_buffer);
  if (IsBreakEventMessage(print_buffer)) {
    // Lets test script wait until break occurs to send commands.
    // Signals when a break is reported.
    message_queue_barriers.semaphore_2->Signal();
  }
  // Allow message handler to block on a semaphore, to test queueing of
  // messages while blocked.
  message_queue_barriers.semaphore_1->Wait();
  printf("%s\n", print_buffer);
  fflush(stdout);
}


void MessageQueueDebuggerThread::Run() {
  const int kBufferSize = 1000;
  uint16_t buffer_1[kBufferSize];
  uint16_t buffer_2[kBufferSize];
  const char* command_1 =
      "{\"seq\":117,"
       "\"type\":\"request\","
       "\"command\":\"evaluate\","
       "\"arguments\":{\"expression\":\"1+2\"}}";
  const char* command_2 =
    "{\"seq\":118,"
     "\"type\":\"request\","
     "\"command\":\"evaluate\","
     "\"arguments\":{\"expression\":\"1+a\"}}";
  const char* command_3 =
    "{\"seq\":119,"
     "\"type\":\"request\","
     "\"command\":\"evaluate\","
     "\"arguments\":{\"expression\":\"c.d * b\"}}";
  const char* command_continue =
    "{\"seq\":106,"
     "\"type\":\"request\","
     "\"command\":\"continue\"}";
  const char* command_single_step =
    "{\"seq\":107,"
     "\"type\":\"request\","
     "\"command\":\"continue\","
     "\"arguments\":{\"stepaction\":\"next\"}}";

  /* Interleaved sequence of actions by the two threads:*/
  // Main thread compiles and runs source_1
  message_queue_barriers.barrier_1.Wait();
  // Post 6 commands, filling the command queue and making it expand.
  // These calls return immediately, but the commands stay on the queue
  // until the execution of source_2.
  // Note: AsciiToUtf16 executes before SendCommand, so command is copied
  // to buffer before buffer is sent to SendCommand.
  v8::Debug::SendCommand(buffer_1, AsciiToUtf16(command_1, buffer_1));
  v8::Debug::SendCommand(buffer_2, AsciiToUtf16(command_2, buffer_2));
  v8::Debug::SendCommand(buffer_2, AsciiToUtf16(command_3, buffer_2));
  v8::Debug::SendCommand(buffer_2, AsciiToUtf16(command_3, buffer_2));
  v8::Debug::SendCommand(buffer_2, AsciiToUtf16(command_3, buffer_2));
  v8::Debug::SendCommand(buffer_2, AsciiToUtf16(command_continue, buffer_2));
  message_queue_barriers.barrier_2.Wait();
  // Main thread compiles and runs source_2.
  // Queued commands are executed at the start of compilation of source_2.
  message_queue_barriers.barrier_3.Wait();
  // Free the message handler to process all the messages from the queue.
  for (int i = 0; i < 20 ; ++i) {
    message_queue_barriers.semaphore_1->Signal();
  }
  // Main thread compiles and runs source_3.
  // source_3 includes a debugger statement, which causes a break event.
  // Wait on break event from hitting "debugger" statement
  message_queue_barriers.semaphore_2->Wait();
  // These should execute after the "debugger" statement in source_2
  v8::Debug::SendCommand(buffer_2, AsciiToUtf16(command_single_step, buffer_2));
  // Wait on break event after a single step executes.
  message_queue_barriers.semaphore_2->Wait();
  v8::Debug::SendCommand(buffer_1, AsciiToUtf16(command_2, buffer_1));
  v8::Debug::SendCommand(buffer_2, AsciiToUtf16(command_continue, buffer_2));
  // Main thread continues running source_3 to end, waits for this thread.
}

MessageQueueDebuggerThread message_queue_debugger_thread;

// This thread runs the v8 engine.
TEST(MessageQueues) {
  // Create a V8 environment
  v8::HandleScope scope;
  DebugLocalContext env;
  message_queue_barriers.Initialize();
  v8::Debug::SetMessageHandler(MessageHandler);
  message_queue_debugger_thread.Start();

  const char* source_1 = "a = 3; b = 4; c = new Object(); c.d = 5;";
  const char* source_2 = "e = 17;";
  const char* source_3 = "a = 4; debugger; a = 5; a = 6; a = 7;";

  // See MessageQueueDebuggerThread::Run for interleaved sequence of
  // API calls and events in the two threads.
  CompileRun(source_1);
  message_queue_barriers.barrier_1.Wait();
  message_queue_barriers.barrier_2.Wait();
  v8::Debug::DebugBreak();
  CompileRun(source_2);
  message_queue_barriers.barrier_3.Wait();
  CompileRun(source_3);
  message_queue_debugger_thread.Join();
  fflush(stdout);
}

/* Test ThreadedDebugging */
/* This test interrupts a running infinite loop that is
 * occupying the v8 thread by a break command from the
 * debugger thread.  It then changes the value of a
 * global object, to make the loop terminate.
 */

Barriers threaded_debugging_barriers;

class V8Thread : public v8::internal::Thread {
 public:
  void Run();
};

class DebuggerThread : public v8::internal::Thread {
 public:
  void Run();
};


static void ThreadedMessageHandler(const uint16_t* message, int length,
                                   void *data) {
  static char print_buffer[1000];
  Utf16ToAscii(message, length, print_buffer);
  if (IsBreakEventMessage(print_buffer)) {
    threaded_debugging_barriers.barrier_2.Wait();
  }
  printf("%s\n", print_buffer);
  fflush(stdout);
}


void V8Thread::Run() {
  const char* source_1 =
      "flag = true;\n"
      "function bar( new_value ) {\n"
      "  flag = new_value;\n"
      "  return \"Return from bar(\" + new_value + \")\";\n"
      "}\n"
      "\n"
      "function foo() {\n"
      "  var x = 1;\n"
      "  while ( flag == true ) {\n"
      "    x = x + 1;\n"
      "  }\n"
      "}\n"
      "\n";
  const char* source_2 = "foo();\n";

  v8::HandleScope scope;
  DebugLocalContext env;
  v8::Debug::SetMessageHandler(&ThreadedMessageHandler);

  CompileRun(source_1);
  threaded_debugging_barriers.barrier_1.Wait();
  CompileRun(source_2);
}

void DebuggerThread::Run() {
  const int kBufSize = 1000;
  uint16_t buffer[kBufSize];

  const char* command_1 = "{\"seq\":102,"
      "\"type\":\"request\","
      "\"command\":\"evaluate\","
      "\"arguments\":{\"expression\":\"bar(false)\"}}";
  const char* command_2 = "{\"seq\":103,"
      "\"type\":\"request\","
      "\"command\":\"continue\"}";

  threaded_debugging_barriers.barrier_1.Wait();
  v8::Debug::DebugBreak();
  threaded_debugging_barriers.barrier_2.Wait();
  v8::Debug::SendCommand(buffer, AsciiToUtf16(command_1, buffer));
  v8::Debug::SendCommand(buffer, AsciiToUtf16(command_2, buffer));
}

DebuggerThread debugger_thread;
V8Thread v8_thread;

TEST(ThreadedDebugging) {
  // Create a V8 environment
  threaded_debugging_barriers.Initialize();

  v8_thread.Start();
  debugger_thread.Start();

  v8_thread.Join();
  debugger_thread.Join();
}

/* Test RecursiveBreakpoints */
/* In this test, the debugger evaluates a function with a breakpoint, after
 * hitting a breakpoint in another function.  We do this with both values
 * of the flag enabling recursive breakpoints, and verify that the second
 * breakpoint is hit when enabled, and missed when disabled.
 */

class BreakpointsV8Thread : public v8::internal::Thread {
 public:
  void Run();
};

class BreakpointsDebuggerThread : public v8::internal::Thread {
 public:
  void Run();
};


Barriers* breakpoints_barriers;

static void BreakpointsMessageHandler(const uint16_t* message,
                                      int length,
                                      void *data) {
  static char print_buffer[1000];
  Utf16ToAscii(message, length, print_buffer);
  printf("%s\n", print_buffer);
  fflush(stdout);

  // Is break_template a prefix of the message?
  if (IsBreakEventMessage(print_buffer)) {
    breakpoints_barriers->semaphore_1->Signal();
  }
}


void BreakpointsV8Thread::Run() {
  const char* source_1 = "var y_global = 3;\n"
    "function cat( new_value ) {\n"
    "  var x = new_value;\n"
    "  y_global = 4;\n"
    "  x = 3 * x + 1;\n"
    "  y_global = 5;\n"
    "  return x;\n"
    "}\n"
    "\n"
    "function dog() {\n"
    "  var x = 1;\n"
    "  x = y_global;"
    "  var z = 3;"
    "  x += 100;\n"
    "  return x;\n"
    "}\n"
    "\n";
  const char* source_2 = "cat(17);\n"
    "cat(19);\n";

  v8::HandleScope scope;
  DebugLocalContext env;
  v8::Debug::SetMessageHandler(&BreakpointsMessageHandler);

  CompileRun(source_1);
  breakpoints_barriers->barrier_1.Wait();
  breakpoints_barriers->barrier_2.Wait();
  CompileRun(source_2);
}


void BreakpointsDebuggerThread::Run() {
  const int kBufSize = 1000;
  uint16_t buffer[kBufSize];

  const char* command_1 = "{\"seq\":101,"
      "\"type\":\"request\","
      "\"command\":\"setbreakpoint\","
      "\"arguments\":{\"type\":\"function\",\"target\":\"cat\",\"line\":3}}";
  const char* command_2 = "{\"seq\":102,"
      "\"type\":\"request\","
      "\"command\":\"setbreakpoint\","
      "\"arguments\":{\"type\":\"function\",\"target\":\"dog\",\"line\":3}}";
  const char* command_3 = "{\"seq\":103,"
      "\"type\":\"request\","
      "\"command\":\"continue\"}";
  const char* command_4 = "{\"seq\":104,"
      "\"type\":\"request\","
      "\"command\":\"evaluate\","
      "\"arguments\":{\"expression\":\"dog()\",\"disable_break\":false}}";
  const char* command_5 = "{\"seq\":105,"
      "\"type\":\"request\","
      "\"command\":\"evaluate\","
      "\"arguments\":{\"expression\":\"x\",\"disable_break\":true}}";
  const char* command_6 = "{\"seq\":106,"
      "\"type\":\"request\","
      "\"command\":\"continue\"}";
  const char* command_7 = "{\"seq\":107,"
      "\"type\":\"request\","
      "\"command\":\"continue\"}";
  const char* command_8 = "{\"seq\":108,"
     "\"type\":\"request\","
     "\"command\":\"evaluate\","
     "\"arguments\":{\"expression\":\"dog()\",\"disable_break\":true}}";
  const char* command_9 = "{\"seq\":109,"
      "\"type\":\"request\","
      "\"command\":\"continue\"}";


  // v8 thread initializes, runs source_1
  breakpoints_barriers->barrier_1.Wait();
  // 1:Set breakpoint in cat().
  v8::Debug::DebugBreak();
  v8::Debug::SendCommand(buffer, AsciiToUtf16(command_1, buffer));
  // 2:Set breakpoint in dog()
  v8::Debug::SendCommand(buffer, AsciiToUtf16(command_2, buffer));
  // 3:Continue
  v8::Debug::SendCommand(buffer, AsciiToUtf16(command_3, buffer));
  breakpoints_barriers->barrier_2.Wait();
  // v8 thread starts compiling source_2.
  // Automatic break happens, to run queued commands
  // breakpoints_barriers->semaphore_1->Wait();
  // Commands 1 through 3 run, thread continues.
  // v8 thread runs source_2 to breakpoint in cat().
  // message callback receives break event.
  breakpoints_barriers->semaphore_1->Wait();
  // 4:Evaluate dog() (which has a breakpoint).
  v8::Debug::SendCommand(buffer, AsciiToUtf16(command_4, buffer));
  // v8 thread hits breakpoint in dog()
  breakpoints_barriers->semaphore_1->Wait();  // wait for break event
  // 5:Evaluate x
  v8::Debug::SendCommand(buffer, AsciiToUtf16(command_5, buffer));
  // 6:Continue evaluation of dog()
  v8::Debug::SendCommand(buffer, AsciiToUtf16(command_6, buffer));
  // dog() finishes.
  // 7:Continue evaluation of source_2, finish cat(17), hit breakpoint
  // in cat(19).
  v8::Debug::SendCommand(buffer, AsciiToUtf16(command_7, buffer));
  // message callback gets break event
  breakpoints_barriers->semaphore_1->Wait();  // wait for break event
  // 8: Evaluate dog() with breaks disabled
  v8::Debug::SendCommand(buffer, AsciiToUtf16(command_8, buffer));
  // 9: Continue evaluation of source2, reach end.
  v8::Debug::SendCommand(buffer, AsciiToUtf16(command_9, buffer));
}

BreakpointsDebuggerThread breakpoints_debugger_thread;
BreakpointsV8Thread breakpoints_v8_thread;

TEST(RecursiveBreakpoints) {
  // Create a V8 environment
  Barriers stack_allocated_breakpoints_barriers;
  stack_allocated_breakpoints_barriers.Initialize();
  breakpoints_barriers = &stack_allocated_breakpoints_barriers;

  breakpoints_v8_thread.Start();
  breakpoints_debugger_thread.Start();

  breakpoints_v8_thread.Join();
  breakpoints_debugger_thread.Join();
}
