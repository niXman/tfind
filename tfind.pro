QT -= core gui

TARGET = tfind
CONFIG += console
CONFIG -= app_bundle qt

TEMPLATE = app

QMAKE_CXXFLAGS += \
	-std=c++11

SOURCES += main.cpp

LIBS += \
	-lboost_system \
	-lboost_filesystem \
	-lboost_thread \
	-lpthread \
	-lrt \
	-lboost_program_options \
	-lboost_regex
