#!/usr/bin/env python3
"""Check that zdxsv sources are listed alike in CMakeLists.txt, the .vcxproj and its .vcxproj.filters.

Every .vcxproj.filters must also parse as XML. Usage: check-vs-projects.py [repo root]
"""

import os
import re
import sys
import xml.etree.ElementTree as ET

NS = "{http://schemas.microsoft.com/developer/msbuild/2003}"
PROJECTS = ("common", "pcsx2", "pcsx2-qt", "pcsx2-gsrunner", "updater")
SOURCE_EXT = (".c", ".cpp", ".h", ".hpp", ".inl", ".ui")


def is_zdxsv(path):
	return any(part.lower().startswith("zdxsv") for part in path.split("/"))


def norm(path):
	return path.replace("\\", "/").lower()


def project_items(path):
	root = ET.parse(path).getroot()
	items = {}
	for group in root.iter(NS + "ItemGroup"):
		for item in group:
			inc = item.attrib.get("Include")
			if inc is None or item.tag == NS + "Filter":
				continue
			f = item.find(NS + "Filter")
			items[(item.tag[len(NS):], norm(inc))] = (inc, f.text if f is not None else None)
	declared = {norm(f.attrib["Include"]) for f in root.iter(NS + "Filter") if "Include" in f.attrib}
	return items, declared


def cmake_sources(path):
	with open(path, encoding="utf-8") as f:
		text = re.sub(r"#[^\n]*", "", f.read())
	return {norm(tok): tok for tok in re.findall(r"[\w./+-]+", text) if tok.endswith(SOURCE_EXT) and "$" not in tok}


def check(d):
	errors = []
	proj = f"{d}/{d}.vcxproj"
	filters = proj + ".filters"
	cmake = f"{d}/CMakeLists.txt"
	if not os.path.exists(proj):
		return errors
	in_proj, _ = project_items(proj)
	if os.path.exists(filters):
		try:
			in_filters, declared = project_items(filters)
		except ET.ParseError as e:
			return [f"{filters}: invalid XML: {e}"]
		for key in sorted(k for k in in_proj.keys() - in_filters.keys() if is_zdxsv(k[1])):
			errors.append(f"{filters}: missing {key[0]} {in_proj[key][0]}")
		for key in sorted(k for k in in_filters.keys() - in_proj.keys() if is_zdxsv(k[1])):
			errors.append(f"{filters}: {key[0]} {in_filters[key][0]} is not in {proj}")
		for key, (inc, flt) in sorted(in_filters.items()):
			if is_zdxsv(key[1]) and flt is not None and norm(flt) not in declared:
				errors.append(f"{filters}: {key[0]} {inc} uses undeclared filter {flt}")
	if os.path.exists(cmake):
		in_cmake = {k: v for k, v in cmake_sources(cmake).items() if is_zdxsv(k)}
		proj_paths = {k[1]: v[0] for k, v in in_proj.items() if is_zdxsv(k[1]) and k[1].endswith(SOURCE_EXT)}
		for p in sorted(in_cmake.keys() - proj_paths.keys()):
			errors.append(f"{proj}: missing {in_cmake[p]} (listed in {cmake})")
		for p in sorted(proj_paths.keys() - in_cmake.keys()):
			errors.append(f"{cmake}: missing {proj_paths[p]} (listed in {proj})")
	return errors


def main():
	os.chdir(sys.argv[1] if len(sys.argv) > 1 else ".")
	errors = []
	for d in PROJECTS:
		errors += check(d)
	for e in errors:
		print(e)
	print(f"{len(errors)} problem(s)")
	return 1 if errors else 0


if __name__ == "__main__":
	sys.exit(main())
