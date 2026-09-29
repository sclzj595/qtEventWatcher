#pragma once

/// QEWT —— QtEventWatcher 单元测试轻量断言头（V6 Q1）
///
/// 设计约束：不引入 QtTest / gtest 依赖（与 TestEventWatcher 的自写断言风格
/// 一致）；仅失败计数 + 行号输出，进程退出码 = 失败数（ctest 冒烟口径一致：
/// exit=0 即全过）。CHECK_EQ 打印实参值便于定位；仅要求 operator== 与流输出。

#include <iostream>
#include <string>

namespace qewt
{

inline int &checks()
{
	static int c = 0;
	return c;
}

inline int &failures()
{
	static int f = 0;
	return f;
}

inline void reportFailure(const char *file, int line, const char *expr)
{
	++failures();
	std::cout << "  [FAIL] " << file << ":" << line << "  CHECK(" << expr << ")\n";
}

template <typename T>
inline void printValue(const T &v) { std::cout << v; }
inline void printValue(const std::string &v) { std::cout << '"' << v << '"'; }
inline void printValue(const char *v) { std::cout << '"' << v << '"'; }
inline void printValue(bool v) { std::cout << (v ? "true" : "false"); }

template <typename A, typename B>
inline void reportEqFailure(const char *file, int line,
                            const char *ea, const char *eb, const A &a, const B &b)
{
	++failures();
	std::cout << "  [FAIL] " << file << ":" << line << "  " << ea << " == " << eb
	          << "  (got: ";
	printValue(a);
	std::cout << " vs ";
	printValue(b);
	std::cout << ")\n";
}

struct Case
{
	const char *name = nullptr;
	void (*fn)() = nullptr;
};

inline int runAll(const Case *cases, int count)
{
	for (int i = 0; i < count; ++i) {
		std::cout << "[ RUN  ] " << cases[i].name << std::endl;
		const int before = failures();
		cases[i].fn();
		std::cout << (failures() == before ? "[  OK  ] " : "[ FAIL ] ")
		          << cases[i].name << std::endl;
	}
	std::cout << "----\nchecks=" << checks() << " failures=" << failures() << std::endl;
	return failures() == 0 ? 0 : 1;
}

} // namespace qewt

#define QEWT_CHECK(cond)                                                        \
	do {                                                                        \
		++qewt::checks();                                                       \
		if (!(cond))                                                            \
			qewt::reportFailure(__FILE__, __LINE__, #cond);                     \
	} while (0)

#define QEWT_CHECK_EQ(a, b)                                                     \
	do {                                                                        \
		++qewt::checks();                                                       \
		auto &&qewt_a_ = (a);                                                   \
		auto &&qewt_b_ = (b);                                                   \
		if (!(qewt_a_ == qewt_b_))                                              \
			qewt::reportEqFailure(__FILE__, __LINE__, #a, #b,                   \
			                      qewt_a_, qewt_b_);                            \
	} while (0)
