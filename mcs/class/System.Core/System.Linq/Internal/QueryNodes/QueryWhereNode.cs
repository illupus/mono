#if NET_4_0
//
// QueryWhereNode.cs
//
// Author:
//       Jérémie "Garuma" Laval <jeremie.laval@gmail.com>
//
// Copyright (c) 2010 Jérémie "Garuma" Laval
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

using System;
using System.Linq;
using System.Threading;
using System.Collections.Generic;

namespace System.Linq
{

	internal class QueryWhereNode<TSource> : QueryStreamNode<TSource, TSource>
	{
		Func<TSource, int, bool> indexedPredicate;
		Func<TSource, bool> predicate;

		internal QueryWhereNode (QueryBaseNode<TSource> parent, Func<TSource, bool> predicate)
			: base (parent, false)
		{
			this.predicate = predicate;
		}

		internal QueryWhereNode (QueryBaseNode<TSource> parent, Func<TSource, int, bool> predicate)
			: base (parent, true)
		{
			this.indexedPredicate = predicate;
		}

		internal override IEnumerable<TSource> GetSequential ()
		{
			IEnumerable<TSource> parent = Parent.GetSequential ();

			if (indexedPredicate != null)
				return parent.Where (indexedPredicate);
			else
				return parent.Where (predicate);
		}

		internal override IList<IEnumerable<TSource>> GetEnumerables (QueryOptions options)
		{
			if (IsIndexed) {
				IList<IEnumerable<KeyValuePair<long, TSource>>> sources = Parent.GetOrderedEnumerables (options);
				IEnumerable<TSource>[] results = new IEnumerable<TSource>[sources.Count];

				for (int i = 0; i < results.Length; i++)
					results[i] =
						sources[i].Where ((e) => indexedPredicate (e.Value, (int)e.Key)).Select ((e) => e.Value);
				return results;
			} else {
				IList<IEnumerable<TSource>> sources = Parent.GetEnumerables (options);
				IEnumerable<TSource>[] results = new IEnumerable<TSource>[sources.Count];

				for (int i = 0; i < results.Length; i++)
					results[i] = sources[i].Where (predicate);
				return results;
			}
		}

		internal override IList<IEnumerable<KeyValuePair<long, TSource>>> GetOrderedEnumerables (QueryOptions options)
		{
			IList<IEnumerable<KeyValuePair<long, TSource>>> sources = Parent.GetOrderedEnumerables (options);
			IEnumerable<KeyValuePair<long, TSource>>[] results = new IEnumerable<KeyValuePair<long, TSource>>[sources.Count];

			Tuple<TSource, long, bool>[] store = new Tuple<TSource, long, bool>[results.Length];
			long lastIndex = 0;

			Barrier barrier = new Barrier (results.Length, delegate (Barrier b) {
				// Sort the store
				Array.Sort (store, ArraySortMethod);

				// Reassign a good index
				int i = 0;

				for (i = 0; i < store.Length && store[i].Item3; i++) {
					Tuple<TSource, long, bool> old = store[i];
					store[i] = Tuple.Create (old.Item1, lastIndex + i, old.Item3);
				}

				// Update lastIndex for next round
				lastIndex += i;
			});

			for (int j = 0; j < results.Length; j++)
				results[j] = GetEnumerator (sources[j], barrier, store, j);

			return results;
		}

		static int ArraySortMethod (Tuple<TSource, long, bool> lhs, Tuple<TSource, long, bool> rhs)
		{
			if (lhs.Item3 && !rhs.Item3)
				return -1;
			if (!lhs.Item3 && rhs.Item3)
				return 1;
			if (!lhs.Item3 && !rhs.Item3)
				return 0;

			return (lhs.Item2 < rhs.Item2) ? -1 : 1;
		}

		IEnumerable<KeyValuePair<long, TSource>> GetEnumerator (IEnumerable<KeyValuePair<long, TSource>> source,
		                                                        Barrier barrier,
		                                                        Tuple<TSource, long, bool>[] store, int index)
		{
			IEnumerator<KeyValuePair<long, TSource>> current = source.GetEnumerator ();
			bool result;

			while (current.MoveNext ()) {
				KeyValuePair<long, TSource> curr = current.Current;

				if (IsIndexed)
					result = indexedPredicate (curr.Value, (int)curr.Key);
				else
					result = predicate (curr.Value);

				store[index] = Tuple.Create (curr.Value, curr.Key, result);
				barrier.SignalAndWait ();

				Tuple<TSource, long, bool> value = store [index];

				if (value.Item3)
					yield return new KeyValuePair<long, TSource> (value.Item2, value.Item1);

				// Reset
				store[index] = Tuple.Create (default (TSource), long.MaxValue, false);
			}

			// Remove our participation
			barrier.RemoveParticipant ();
		}
	}
}
#endif
