#include "Wisp/Components/ValueBox.h"
#include <cctype>
#include <cstdlib>

namespace wisp {

static void SkipWhitespace(const char*& s)
{
	while (*s && std::isspace((unsigned char)*s)) s++;
}

template<typename T>
bool TValueBox<T>::EvalFactor(const char*& s, double& out)
{
	SkipWhitespace(s);
	if (*s == '(')
	{
		s++;
		if (!EvalExpr(s, out)) return false;
		SkipWhitespace(s);
		if (*s != ')') return false;
		s++;
		return true;
	}
	if (*s == '+' || *s == '-')
	{
		bool neg = (*s == '-');
		s++;
		if (!EvalFactor(s, out)) return false;
		if (neg) out = -out;
		return true;
	}
	char* end = nullptr;
	out = std::strtod(s, &end);
	if (end == s) return false;
	s = end;
	return true;
}

template<typename T>
bool TValueBox<T>::EvalTerm(const char*& s, double& out)
{
	if (!EvalFactor(s, out)) return false;
	while (true)
	{
		SkipWhitespace(s);
		if (*s == '*' || *s == '/')
		{
			char op = *s;
			s++;
			double rhs = 0.0;
			if (!EvalFactor(s, rhs)) return false;
			if (op == '*') out *= rhs;
			else { if (rhs == 0.0) return false; out /= rhs; }
		}
		else break;
	}
	return true;
}

template<typename T>
bool TValueBox<T>::EvalExpr(const char*& s, double& out)
{
	if (!EvalTerm(s, out)) return false;
	while (true)
	{
		SkipWhitespace(s);
		if (*s == '+' || *s == '-')
		{
			char op = *s;
			s++;
			double rhs = 0.0;
			if (!EvalTerm(s, rhs)) return false;
			if (op == '+') out += rhs;
			else out -= rhs;
		}
		else break;
	}
	return true;
}

template bool TValueBox<float>::EvalExpr(const char*&, double&);
template bool TValueBox<float>::EvalTerm(const char*&, double&);
template bool TValueBox<float>::EvalFactor(const char*&, double&);
template bool TValueBox<int>::EvalExpr(const char*&, double&);
template bool TValueBox<int>::EvalTerm(const char*&, double&);
template bool TValueBox<int>::EvalFactor(const char*&, double&);

} // namespace wisp
