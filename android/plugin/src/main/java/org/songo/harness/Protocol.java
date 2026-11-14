package org.songo.harness;

import java.util.ArrayList;

/**
 * The engine's protocol lines (common/protocol.h): tab separated fields, with
 * backslash, tab, newline and carriage return escaped.
 */
final class Protocol {
	private Protocol() {}

	static String[] split(String line) {
		ArrayList<String> fields = new ArrayList<>();
		StringBuilder field = new StringBuilder();
		for (int i = 0; i < line.length(); i++) {
			char c = line.charAt(i);
			if (c == '\t') {
				fields.add(field.toString());
				field.setLength(0);
			} else if (c == '\\' && i + 1 < line.length()) {
				char next = line.charAt(++i);
				field.append(next == 't' ? '\t' : next == 'n' ? '\n' : next == 'r' ? '\r' : next);
			} else {
				field.append(c);
			}
		}
		fields.add(field.toString());
		return fields.toArray(new String[0]);
	}

	static String field(String[] fields, int i) {
		return i < fields.length ? fields[i] : "";
	}

	static int intField(String[] fields, int i) {
		try {
			return Integer.parseInt(field(fields, i));
		} catch (NumberFormatException e) {
			return 0;
		}
	}

	static double doubleField(String[] fields, int i) {
		try {
			return Double.parseDouble(field(fields, i));
		} catch (NumberFormatException e) {
			return 0.0;
		}
	}
}
