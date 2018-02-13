# =============================================================================
# 2013+ Copyright (c) Kirill Smorodinnikov <shaitkir@gmail.com>
# 2013+ Copyright (c) Alexey Ivanov <rbtz@ph34r.me>
# All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# =============================================================================

"""
Stats for humans (c)
Simple and Python-ish interface to stats.
Currently we support counters and time measurements.
"""

import itertools
from datetime import datetime
try:
    from collections import OrderedDict
except:
    OrderedDict = dict


def format_kv(k, v):
    """Formats one line of test output"""
    return '{0:<50}{1:>50}'.format(k + ':', str(v))


class ResultCounter(object):
    """
    Counter of successes and failures.

    Use += to increment successes
    Use -= to increment failures
    Totals are computed automagically.
    >>> rc = ResultCounter('Counter')
    >>> rc += 10
    >>> rc -= 2
    >>> print rc
    Counter_failures:                                                                                  2
    Counter_total:                                                                                    12
    Counter_success:                                                                                  10
    """

    def __init__(self, name, success=0, failures=0):
        self.name = name
        self.success = success
        self.failures = failures

    def __iadd__(self, other):
        self.success += other
        return self

    def __isub__(self, other):
        self.failures += other
        return self

    @property
    def total(self):
        return self.failures + self.success

    def __str__(self):
        result = []
        if self.failures:
            result.append(format_kv(self.name + '_success', self.success))
            result.append(format_kv(self.name + '_failures', self.failures))
            result.append(format_kv(self.name + '_total', self.total))
        else:
            result.append(format_kv(self.name, self.success))
        return "\n".join(result)

    def dump_to_dict(self):
        if self.failures:
            result = OrderedDict()
            result['successes'] = self.success
            result['failures'] = self.failures
            result['total'] = self.total
            return result
        else:
            return self.success


class DurationTimer(object):
    """
    Time measurement between events and reporting.
    TODO: Add context-manager interface

    One can mark different points in program execution with timer
    >>> dt = DurationTimer('Timer')
    >>> dt('start')
    >>> dt('work')
    >>> dt('stop')

    It also has pretty printing of it's content split by intervals
    >>> str(dt) != ""
    True
    """

    def __init__(self, name):
        self.name = name
        self.times = []

    def __call__(self, name=None, ts=None):
        if ts is None:
            ts = datetime.now()
        self.times.append((name, ts))

    def __str__(self):
        if not self.times:
            return ""

        def construct_line(times):
            measure_name, measure_time = times
            result = str(self.name) + "_" + str(measure_name)
            return format_kv(result, measure_time)

        result = []
        start, stop = self.times[0], self.times[-1]
        result.append(construct_line(start))
        for begin, end in zip(self.times, self.times[1:]):
            name = str(begin[0]) + "..." + str(end[0])
            time = end[1] - begin[1]
            result.append(construct_line((name, time)))
        if start != stop:
            result.append(construct_line(stop))
        return "\n".join(result)

    def dump_to_dict(self):
        if not self.times:
            return

        result = OrderedDict()
        name, time = self.times[0]
        result[str(name)] = str(time)
        for (begin_name, begin_time), (end_name, end_time) in zip(self.times, self.times[1:]):
            name = '{0}...{1}'.format(begin_name, end_name)
            result[name] = str(end_time - begin_time)
            result[end_name] = str(end_time)

        return result


class AttributeContainer(object):
    """List of different values combined under an unique name.

    Use .append(value) to @append value to the attribute.
    >>> ac = AttributeContainer('Attribute')
    >>> ac.append(10)
    >>> ac.append("some")
    >>> print ac
    Attribute:                                                                              [10, 'some']
    """
    def __init__(self, name):
        self.name = name
        self.attributes = []

    def append(self, value):
        self.attributes.append(value)

    def __attributes(self):
        if not self.attributes:
            return
        elif len(self.attributes) == 1:
            return self.attributes[0]
        else:
            return self.attributes

    def __str__(self):
        dump = self.__dump()
        if not dump:
            return ''
        else:
            return format_kv(self.name, dump) + '\n'

    def __dump(self):
        if not self.attributes:
            return
        elif len(self.attributes) == 1:
            return self.attributes[0]
        else:
            return self.attributes

    def dump_to_dict(self):
        return self.__dump()


class Container(object):
    """
    Container class which creates instance of provided `klass` on attribute access.
    """

    def __init__(self, klass, *args, **kwargs):
        self.__klass = klass
        self.__args = args
        self.__kwargs = kwargs
        self.__container = dict()

    def __getattr__(self, item):
        if item.startswith('_'):
            raise AttributeError("Attribute not found: {0}".format(item))
        if item not in self.__container:
            self.__container[item] = self.__klass(name=item, *self.__args, **self.__kwargs)
        return self.__container[item]

    def __setattr__(self, key, value):
        if not key.startswith('_'):
            self.__container[key] = value
        else:
            self.__dict__[key] = value

    def __iter__(self):
        return self.__container.iteritems()


class Stats(object):
    """
    Very simple statistics with nesting.
    For now it supports counters and timers.


    Each Stat instance has a name:
    >>> stats = Stats('global')

    Stat instance have built-in counter factory
    >>> stats.counter.test_local += 1

    Also you can create any number of nested stats on demand
    >>> stats['sub_stat'].counter.test_nested -= 1

    Stats have pretty-printing of it's content
    >>> print stats
    ============================================== global ==============================================
    test_local_total:                                                                                  1
    test_local_success:                                                                                1
    ============================================= sub_stat =============================================
    test_nested_failures:                                                                              1
    test_nested_total:                                                                                 1

    >>> stat_name = 'sub_stat_2'
    >>> plugin_stat = Stats(stat_name)
    >>> plugin_stat.counter.failure -= 1
    >>> stats[stat_name] = plugin_stat
    >>> print stats
    ============================================== global ==============================================
    test_local_total:                                                                                  1
    test_local_success:                                                                                1
    ============================================= sub_stat =============================================
    test_nested_failures:                                                                              1
    test_nested_total:                                                                                 1
    ============================================ sub_stat_2 ============================================
    failure_failures:                                                                                  1
    failure_total:                                                                                     1

    Also Stat instance has builtin timer factory
    >>> stats.timer.test('start')
    """

    def __init__(self, name=None):
        self.name = name
        self.counter = Container(ResultCounter)
        self.timer = Container(DurationTimer)
        self.attributes = Container(AttributeContainer)
        self.__sub_stats = Container(Stats)

    def __containers(self):
        return itertools.chain(sorted(self.counter), sorted(self.timer), sorted(self.attributes), sorted(self.__sub_stats))

    def __str__(self):
        result = ["{0:=^100}".format(" " + str(self.name) + " ")]
        for _, v in self.__containers():
            result.append(str(v))
        return "\n".join(result)

    def dump_to_dict(self):
        result = OrderedDict()
        for _, v in self.__containers():
            result[v.name] = v.dump_to_dict()
        return result

    def json(self):
        import json
        return json.dumps(self.dump_to_dict(), sort_keys=False,
                          indent=4, separators=(',', ': '))

    def __getitem__(self, item):
        return getattr(self.__sub_stats, str(item))

    def __setitem__(self, key, value):
        setattr(self.__sub_stats, key, value)
