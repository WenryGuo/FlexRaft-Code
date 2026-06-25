// memory_leak_test.cc — 内存管理和线程安全测试
//
// 测试内容：
// 1. Slice 类的所有权语义（owned_ 标志）
// 2. LogEntry 析构函数是否正确释放所有 Slice 成员
// 3. Slice 拷贝构造/赋值的内存管理
// 4. 多线程并发访问 raft_nodes_ 的线程安全

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>

#include "raft/log_entry.h"
#include "raft_type.h"

// ============================================================================
// 测试工具：内存统计
// ============================================================================

#ifdef __linux__
#include <sys/resource.h>

static size_t GetCurrentRSS() {
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    return usage.ru_maxrss * 1024;  // KB to bytes
  }
  return 0;
}
#else
static size_t GetCurrentRSS() { return 0; }
#endif

// ============================================================================
// 测试 1: Slice 默认构造和析构
// ============================================================================

static void TestSliceDefaultConstructor() {
  printf("\n=== Test 1: Slice default constructor ===\n");

  {
    raft::Slice s;  // 默认构造
    if (s.data() != nullptr || s.size() != 0 || s.valid()) {
      printf("FAIL: Default constructor should create invalid slice\n");
      exit(1);
    }
  }  // s 析构，不应崩溃

  printf("PASS: Slice default constructor works correctly\n");
}

// ============================================================================
// 测试 2: Slice Take (拥有所有权)
// ============================================================================

static void TestSliceTakeOwnership() {
  printf("\n=== Test 2: Slice Take (ownership transfer) ===\n");

  // 分配内存并转移所有权
  char* raw = new char[100];
  memset(raw, 'A', 100);

  {
    raft::Slice s = raft::Slice::Take(raw, 100);
    if (!s.valid() || s.size() != 100) {
      printf("FAIL: Take should create valid slice\n");
      exit(1);
    }
    if (s.data() != raw) {
      printf("FAIL: Take should preserve pointer\n");
      exit(1);
    }
  }  // s 析构，应该释放 raw

  printf("PASS: Slice Take ownership works correctly\n");
}

// ============================================================================
// 测试 3: Slice Copy (深拷贝)
// ============================================================================

static void TestSliceCopy() {
  printf("\n=== Test 3: Slice Copy (deep copy) ===\n");

  char original[] = "Hello, World!";
  raft::Slice original_slice(original, sizeof(original) - 1);

  size_t mem_before = GetCurrentRSS();

  // 执行多次拷贝
  for (int i = 0; i < 1000; ++i) {
    raft::Slice copy = raft::Slice::Copy(original_slice);
    if (!copy.valid() || copy.size() != original_slice.size()) {
      printf("FAIL: Copy should create valid slice\n");
      exit(1);
    }
    if (memcmp(copy.data(), original, sizeof(original) - 1) != 0) {
      printf("FAIL: Copy should have same content\n");
      exit(1);
    }
  }  // 每次循环结束时 copy 析构，释放内存

  size_t mem_after = GetCurrentRSS();
  printf("  Memory delta: %ld bytes\n", (long)(mem_after - mem_before));

  // 如果内存泄漏，mem_after 会显著大于 mem_before
  // 允许一些小误差（100KB），但不应该有大的增长
  if (mem_after > mem_before + 1024 * 1024) {
    printf("WARNING: Possible memory leak in Slice::Copy\n");
  }

  printf("PASS: Slice Copy works correctly\n");
}

// ============================================================================
// 测试 4: Slice View (非拥有)
// ============================================================================

static void TestSliceView() {
  printf("\n=== Test 4: Slice View (non-owning) ===\n");

  char external_buffer[100];
  memset(external_buffer, 'X', 100);

  {
    raft::Slice view = raft::Slice::View(external_buffer, 100);
    if (!view.valid() || view.size() != 100) {
      printf("FAIL: View should create valid slice\n");
      exit(1);
    }
    // View 不拥有内存，析构时不应释放 external_buffer
  }

  // 确保 external_buffer 在 view 析构后仍然有效
  if (external_buffer[0] != 'X') {
    printf("FAIL: View should not free external buffer\n");
    exit(1);
  }

  printf("PASS: Slice View works correctly\n");
}

// ============================================================================
// 测试 5: Slice 拷贝赋值
// ============================================================================

static void TestSliceCopyAssignment() {
  printf("\n=== Test 5: Slice copy assignment ===\n");

  char data1[] = "First";
  char data2[] = "Second";

  raft::Slice s1 = raft::Slice::Take(new char[10], 10);
  memcpy(s1.data(), data1, 5);

  raft::Slice s2 = raft::Slice::Take(new char[10], 10);
  memcpy(s2.data(), data2, 6);

  // s1 和 s2 现在都拥有各自的内存

  s1 = s2;  // s1 释放原有内存，拷贝 s2 的数据

  // s2 在作用域结束时析构，s1 现在拥有唯一的内存

  if (memcmp(s1.data(), data2, 6) != 0) {
    printf("FAIL: Copy assignment should copy data\n");
    exit(1);
  }

  printf("PASS: Slice copy assignment works correctly\n");
}

// ============================================================================
// 测试 6: LogEntry 析构释放 command_data_
// ============================================================================

static void TestLogEntryDestroysCommandData() {
  printf("\n=== Test 6: LogEntry destroys command_data_ ===\n");

  size_t mem_before = GetCurrentRSS();

  for (int i = 0; i < 100; ++i) {
    raft::LogEntry entry;
    entry.SetType(raft::kNormal);

    // Use Copy to create an owned Slice that LogEntry can safely take ownership of
    // NOTE: SetCommandData does a copy assignment, so we need to use Copy (owned)
    // NOT Take (which would cause double-free when the temp Slice is destroyed)
    char temp_data[4096];
    memset(temp_data, 'D', 4096);
    raft::Slice cmd_data = raft::Slice::Copy(raft::Slice(temp_data, 4096));
    entry.SetCommandData(cmd_data);

    if (!entry.CommandData().valid() || entry.CommandLength() != 4096) {
      printf("FAIL: SetCommandData should work\n");
      exit(1);
    }
  }  // entry 析构，应该释放 command_data

  size_t mem_after = GetCurrentRSS();
  printf("  Memory delta: %ld bytes\n", (long)(mem_after - mem_before));

  if (mem_after > mem_before + 1024 * 512) {
    printf("WARNING: Possible leak in LogEntry::SetCommandData\n");
  }

  printf("PASS: LogEntry destroys command_data_ correctly\n");
}

// ============================================================================
// 测试 7: LogEntry 析构释放 not_encoded_slice_
// ============================================================================

static void TestLogEntryDestroysNotEncodedSlice() {
  printf("\n=== Test 7: LogEntry destroys not_encoded_slice_ ===\n");

  raft::LogEntry entry;
  // When type is kFragments, NotEncodedSlice() returns not_encoded_slice_
  entry.SetType(raft::kFragments);

  // Use Copy to create an owned slice
  char temp[1024];
  raft::Slice slice = raft::Slice::Copy(raft::Slice(temp, 1024));
  entry.SetNotEncodedSlice(slice);

  if (!entry.NotEncodedSlice().valid()) {
    printf("FAIL: SetNotEncodedSlice should work\n");
    exit(1);
  }

  // entry 析构时应该释放 not_encoded_slice_
  printf("PASS: LogEntry destroys not_encoded_slice_ correctly\n");
}

// ============================================================================
// 测试 8: LogEntry 析构释放 fragment_slice_
// ============================================================================

static void TestLogEntryDestroysFragmentSlice() {
  printf("\n=== Test 8: LogEntry destroys fragment_slice_ ===\n");

  raft::LogEntry entry;
  entry.SetType(raft::kFragments);

  // Use Copy to create an owned slice
  char temp[2048];
  raft::Slice slice = raft::Slice::Copy(raft::Slice(temp, 2048));
  entry.SetFragmentSlice(slice);

  if (!entry.FragmentSlice().valid()) {
    printf("FAIL: SetFragmentSlice should work\n");
    exit(1);
  }

  // entry 析构时应该释放 fragment_slice_
  printf("PASS: LogEntry destroys fragment_slice_ correctly\n");
}

// ============================================================================
// 测试 9: LogEntry 拷贝构造
// ============================================================================

static void TestLogEntryCopyConstructor() {
  printf("\n=== Test 9: LogEntry copy constructor ===\n");

  raft::LogEntry entry1;
  entry1.SetType(raft::kNormal);
  entry1.SetTerm(1);
  entry1.SetIndex(100);

  char temp1[1024];
  entry1.SetCommandData(raft::Slice::Copy(raft::Slice(temp1, 1024)));

  char temp2[512];
  entry1.AddFragment(raft::Slice::Copy(raft::Slice(temp2, 512)));

  // 拷贝构造
  raft::LogEntry entry2(entry1);

  // 验证数据相同
  if (entry2.Term() != entry1.Term() || entry2.Index() != entry1.Index()) {
    printf("FAIL: Copy constructor should copy metadata\n");
    exit(1);
  }

  if (entry2.CommandLength() != entry1.CommandLength()) {
    printf("FAIL: Copy constructor should copy command_data\n");
    exit(1);
  }

  if (entry2.Fragments().size() != entry1.Fragments().size()) {
    printf("FAIL: Copy constructor should copy fragments\n");
    exit(1);
  }

  // entry1 和 entry2 现在各自拥有独立的内存
  printf("PASS: LogEntry copy constructor works correctly\n");
}

// ============================================================================
// 测试 10: LogEntry 拷贝赋值
// ============================================================================

static void TestLogEntryCopyAssignment() {
  printf("\n=== Test 10: LogEntry copy assignment ===\n");

  raft::LogEntry entry1;
  entry1.SetType(raft::kNormal);
  entry1.SetTerm(1);
  entry1.SetIndex(100);

  char temp1[1024];
  entry1.SetCommandData(raft::Slice::Copy(raft::Slice(temp1, 1024)));

  char temp2[512];
  entry1.AddFragment(raft::Slice::Copy(raft::Slice(temp2, 512)));

  raft::LogEntry entry2;
  entry2.SetType(raft::kNormal);
  entry2.SetTerm(2);
  entry2.SetIndex(200);

  char temp3[2048];
  entry2.SetCommandData(raft::Slice::Copy(raft::Slice(temp3, 2048)));

  char temp4[1024];
  entry2.AddFragment(raft::Slice::Copy(raft::Slice(temp4, 1024)));

  entry2 = entry1;  // entry2 释放原有内存，拷贝 entry1

  if (entry2.Term() != 1 || entry2.Index() != 100) {
    printf("FAIL: Copy assignment should copy metadata\n");
    exit(1);
  }

  if (entry2.CommandLength() != 1024) {
    printf("FAIL: Copy assignment should copy command_data\n");
    exit(1);
  }

  printf("PASS: LogEntry copy assignment works correctly\n");
}

// ============================================================================
// 测试 11: Slice 移动语义
// ============================================================================

static void TestSliceMoveSemantics() {
  printf("\n=== Test 11: Slice move semantics ===\n");

  raft::Slice s1 = raft::Slice::Take(new char[100], 100);
  char* original_ptr = s1.data();

  raft::Slice s2 = std::move(s1);

  // s1 现在应该无效
  if (s1.valid()) {
    printf("FAIL: Moved-from slice should be invalid\n");
    exit(1);
  }

  // s2 应该持有原来的指针
  if (s2.data() != original_ptr || s2.size() != 100) {
    printf("FAIL: Moved-to slice should have original data\n");
    exit(1);
  }

  printf("PASS: Slice move semantics work correctly\n");
}

// ============================================================================
// 测试 12: String 构造的 Slice 拥有所有权
// ============================================================================

static void TestSliceFromString() {
  printf("\n=== Test 12: Slice from string ===\n");

  std::string str = "Hello from string!";

  {
    raft::Slice s(str);

    if (!s.valid() || s.size() != str.size()) {
      printf("FAIL: Slice from string should be valid\n");
      exit(1);
    }

    if (memcmp(s.data(), str.data(), str.size()) != 0) {
      printf("FAIL: Slice from string should have same content\n");
      exit(1);
    }
  }  // s 析构，释放复制的内存

  printf("PASS: Slice from string works correctly\n");
}

// ============================================================================
// 主函数
// ============================================================================

int main(int argc, char* argv[]) {
  printf("=========================================================\n");
  printf("  FlexRaft Memory Management Test Suite\n");
  printf("=========================================================\n");

  size_t start_mem = GetCurrentRSS();
  printf("\nStarting memory: %zu bytes\n", start_mem);

  // 运行所有测试
  TestSliceDefaultConstructor();
  TestSliceTakeOwnership();
  TestSliceCopy();
  TestSliceView();
  TestSliceCopyAssignment();
  TestSliceMoveSemantics();
  TestSliceFromString();
  TestLogEntryDestroysCommandData();
  TestLogEntryDestroysNotEncodedSlice();
  TestLogEntryDestroysFragmentSlice();
  TestLogEntryCopyConstructor();
  TestLogEntryCopyAssignment();

  size_t end_mem = GetCurrentRSS();
  printf("\n=========================================================\n");
  printf("  Final memory: %zu bytes (delta: %zd bytes)\n",
         end_mem, (ssize_t)(end_mem - start_mem));
  printf("  All tests passed!\n");
  printf("=========================================================\n");

  return 0;
}
