using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// Keyed in-place synchronisation of an <see cref="ObservableCollection{T}"/> against an incoming
/// ordered list — the ONE place the 0xc000027b rule ("never <c>Clear()</c>+<c>Add</c> a bound
/// collection; update rows in place; insert/remove individually") is implemented for the OHG
/// workspace, so every keyed collection (panelists, slots, gallery, unseated, and Task 5's look
/// boxes) obeys it identically.
///
/// Contract:
/// <list type="bullet">
/// <item>A key present in both the collection and <paramref name="incoming"/> keeps its EXISTING
/// item instance and is refreshed via <c>update(item, incoming)</c> — bindings stay attached and a
/// row VM's <c>[ObservableProperty]</c> setters raise only for values that actually changed.</item>
/// <item>A key only in <paramref name="incoming"/> is created and <c>Insert</c>ed at its position
/// in the incoming order (the projection emits sorted lists, so that IS the sorted position).</item>
/// <item>A key only in the collection is <c>RemoveAt</c>ed.</item>
/// <item>A surviving key that moved is <c>Move</c>d, never removed and re-added.</item>
/// <item>Every structural change raises exactly one <c>CollectionChanged</c> — Add, Remove or
/// Move. <c>Reset</c> is NEVER raised, which is precisely the <c>Clear()</c> signature that
/// fail-fasts WinUI's CoreMessagingXP under churn.</item>
/// </list>
/// </summary>
internal static class ObservableCollectionSync
{
    /// <summary>Syncs <paramref name="collection"/> to <paramref name="incoming"/> in place.
    /// Keys must be unique within <paramref name="incoming"/>; a duplicate key is dropped (the
    /// first wins) rather than corrupting the collection.</summary>
    public static void Apply<TKey, TItem, TIncoming>(
        ObservableCollection<TItem> collection,
        IReadOnlyList<TIncoming> incoming,
        Func<TItem, TKey> keyOfItem,
        Func<TIncoming, TKey> keyOfIncoming,
        Func<TIncoming, TItem> create,
        Action<TItem, TIncoming> update)
        where TKey : notnull
    {
        ArgumentNullException.ThrowIfNull(collection);
        ArgumentNullException.ThrowIfNull(incoming);

        // The desired key order, deduped.
        var desired = new List<TIncoming>(incoming.Count);
        var desiredKeys = new HashSet<TKey>();
        foreach (var entry in incoming)
        {
            if (desiredKeys.Add(keyOfIncoming(entry))) desired.Add(entry);
        }

        // 1. Remove departed keys (individually, back to front so indices stay valid).
        for (var i = collection.Count - 1; i >= 0; i--)
        {
            if (!desiredKeys.Contains(keyOfItem(collection[i])))
            {
                collection.RemoveAt(i);
            }
        }

        // 2. Walk the desired order, moving/inserting so position i holds key i.
        for (var i = 0; i < desired.Count; i++)
        {
            var entry = desired[i];
            var key = keyOfIncoming(entry);

            if (i < collection.Count && EqualityComparer<TKey>.Default.Equals(keyOfItem(collection[i]), key))
            {
                update(collection[i], entry);
                continue;
            }

            var existingIndex = IndexOfKey(collection, keyOfItem, key, i);
            if (existingIndex >= 0)
            {
                collection.Move(existingIndex, i);   // one Move event, never Remove+Add
                update(collection[i], entry);
                continue;
            }

            var created = create(entry);
            update(created, entry);
            collection.Insert(i, created);
        }
    }

    /// <summary>The string flavour: a plain list of lines (restore warnings) kept in place by
    /// index — a changed line is REPLACED at its index (one Replace event), and only the tail
    /// grows or shrinks. Strings carry no identity, so index IS the key here; the alternative
    /// (keying on the string itself) breaks the moment two identical warnings arrive.</summary>
    public static void ApplyStrings(ObservableCollection<string> collection, IReadOnlyList<string> incoming)
    {
        ArgumentNullException.ThrowIfNull(collection);
        ArgumentNullException.ThrowIfNull(incoming);

        for (var i = 0; i < incoming.Count; i++)
        {
            if (i < collection.Count)
            {
                if (!string.Equals(collection[i], incoming[i], StringComparison.Ordinal))
                {
                    collection[i] = incoming[i];
                }
            }
            else
            {
                collection.Add(incoming[i]);
            }
        }

        for (var i = collection.Count - 1; i >= incoming.Count; i--)
        {
            collection.RemoveAt(i);
        }
    }

    private static int IndexOfKey<TKey, TItem>(
        ObservableCollection<TItem> collection, Func<TItem, TKey> keyOfItem, TKey key, int from)
        where TKey : notnull
    {
        for (var i = from; i < collection.Count; i++)
        {
            if (EqualityComparer<TKey>.Default.Equals(keyOfItem(collection[i]), key)) return i;
        }
        return -1;
    }
}
