constexpr int answer = 6 * 7;
int f(void) {
	constexpr long local = 9;
	return answer + (int)local;
}
