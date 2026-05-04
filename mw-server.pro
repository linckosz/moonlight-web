TEMPLATE = subdirs
CONFIG += ordered

third_party.subdir = backend/third_party

SUBDIRS = third_party backend
backend.depends = third_party
