using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.Linq;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>The keyed in-place collection sync every OHG collection rides (Plan 7b Task 3, reused
/// by Task 5's look boxes). The contract that matters is the 0xc000027b one: no <c>Reset</c>, ever.</summary>
public sealed class ObservableCollectionSyncTests
{
    private sealed record Row(string Key, string Value);

    private sealed class Item
    {
        public Item(Row row) { Key = row.Key; Value = row.Value; }
        public string Key { get; }
        public string Value { get; set; }
    }

    private static (ObservableCollection<Item> Collection, List<NotifyCollectionChangedAction> Actions) Seed(params string[] keys)
    {
        var collection = new ObservableCollection<Item>(keys.Select(k => new Item(new Row(k, k))));
        var actions = new List<NotifyCollectionChangedAction>();
        collection.CollectionChanged += (_, e) => actions.Add(e.Action);
        return (collection, actions);
    }

    private static void Sync(ObservableCollection<Item> collection, params Row[] rows)
        => ObservableCollectionSync.Apply(
            collection, rows,
            item => item.Key, row => row.Key,
            row => new Item(row),
            (item, row) => item.Value = row.Value);

    [Fact]
    public void SurvivingKeysKeepTheirInstanceAndAreUpdatedInPlace()
    {
        var (collection, actions) = Seed("a", "b");
        var a = collection[0];

        Sync(collection, new Row("a", "A!"), new Row("b", "b"));

        Assert.Same(a, collection[0]);
        Assert.Equal("A!", collection[0].Value);
        Assert.Empty(actions);   // no structural change at all
    }

    [Fact]
    public void NewKeysAreInsertedAtTheirSortedPositionAndDepartedAreRemoved()
    {
        var (collection, actions) = Seed("b", "d");
        var b = collection[0];

        Sync(collection, new Row("a", "a"), new Row("b", "b"), new Row("c", "c"));

        Assert.Equal(new[] { "a", "b", "c" }, collection.Select(i => i.Key));
        Assert.Same(b, collection[1]);
        Assert.Contains(NotifyCollectionChangedAction.Remove, actions);   // d departed
        Assert.Contains(NotifyCollectionChangedAction.Add, actions);
        Assert.DoesNotContain(NotifyCollectionChangedAction.Reset, actions);
    }

    [Fact]
    public void AReorderIsAMoveNeverARemoveAndReAdd()
    {
        var (collection, actions) = Seed("a", "b", "c");
        var c = collection[2];

        Sync(collection, new Row("c", "c"), new Row("a", "a"), new Row("b", "b"));

        Assert.Equal(new[] { "c", "a", "b" }, collection.Select(i => i.Key));
        Assert.Same(c, collection[0]);
        Assert.Equal(new[] { NotifyCollectionChangedAction.Move }, actions.Distinct());
    }

    [Fact]
    public void EmptyingTheIncomingListRemovesRowsOneByOne_NeverAReset()
    {
        var (collection, actions) = Seed("a", "b", "c");

        Sync(collection);

        Assert.Empty(collection);
        Assert.Equal(3, actions.Count(a => a == NotifyCollectionChangedAction.Remove));
        Assert.DoesNotContain(NotifyCollectionChangedAction.Reset, actions);
    }

    [Fact]
    public void ApplyStringsReplacesInPlaceAndTrimsTheTail()
    {
        var collection = new ObservableCollection<string> { "one", "two", "three" };
        var actions = new List<NotifyCollectionChangedAction>();
        collection.CollectionChanged += (_, e) => actions.Add(e.Action);

        ObservableCollectionSync.ApplyStrings(collection, new[] { "one", "TWO" });

        Assert.Equal(new[] { "one", "TWO" }, collection);
        Assert.Contains(NotifyCollectionChangedAction.Replace, actions);
        Assert.Contains(NotifyCollectionChangedAction.Remove, actions);
        Assert.DoesNotContain(NotifyCollectionChangedAction.Reset, actions);

        ObservableCollectionSync.ApplyStrings(collection, new[] { "one", "TWO", "four" });
        Assert.Equal(new[] { "one", "TWO", "four" }, collection);
    }
}
