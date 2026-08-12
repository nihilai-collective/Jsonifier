// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/main.cpp

#include "tests.hpp"

int32_t main() {
	try {
		std::cout << "Current active CPU backend: " << jsonifier::cpu_arch_name << std::endl;
		tests::testFunction();
	} catch (const std::runtime_error& e) {
		std::cout << "Error: " << e.what() << std::endl;
		return 1;
	} catch (const rt_ut::rt_ut_exception& e) {
		std::cout << "Error: " << e.what() << std::endl;
		return 1;
	}
	return 0;
}
