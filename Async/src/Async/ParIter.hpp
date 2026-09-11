#pragma once

#include <Async/Chunk.hpp>
#include <Async/Ranges.hpp>
#include <Async/Runtime.hpp>
#include <Async/Task.hpp>

#include <functional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>
#include <optional>
#include <algorithm>
#include <numeric>

namespace SFT::Async {

    template <class R>
    concept ParIterable = std::ranges::viewable_range<R> && std::ranges::random_access_range<R> && std::ranges::sized_range<R>;

    template <AsyncRuntime Rt, class V>
        requires std::ranges::view<V> && ParIterable<V>
    class ParIter {
        V view_;

        template <class R> static auto make(R &&r) { auto v=std::views::all(std::forward<R>(r)); return ParIter<Rt,decltype(v)>(std::move(v)); }

      public:
        explicit constexpr ParIter(V view) noexcept(std::is_nothrow_move_constructible_v<V>) : view_(std::move(view)) {}
        [[nodiscard]] usize count() const noexcept { return static_cast<usize>(std::ranges::size(view_)); }

        template <class Fn> requires AsyncWork<Fn, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto map(Fn fn) { auto r=std::views::transform(std::move(view_),std::move(fn)); return ParIter<Rt,decltype(r)>(std::move(r)); }
        template <class Pred> requires std::predicate<Pred &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto filter(Pred pred) { return make(Ranges::filter<Rt>(view_,std::move(pred))); }
        template <class Fn> requires std::invocable<Fn &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto filter_map(Fn fn) {
            using Opt=std::remove_cvref_t<std::invoke_result_t<Fn &,std::ranges::range_reference_t<V>>>; using T=typename Opt::value_type;
            auto mapped=Ranges::map<Rt>(view_,[fn](auto &&x){return fn(std::forward<decltype(x)>(x));}); std::vector<T> out; out.reserve(mapped.size()); for(auto &x:mapped)if(x)out.push_back(*x); return make(std::move(out));
        }
        template <class Fn> requires std::invocable<Fn &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto flat_map(Fn fn) { auto mapped=Ranges::map<Rt>(view_,std::move(fn)); std::vector<std::ranges::range_value_t<std::ranges::range_value_t<decltype(mapped)>>> out; for(auto &r:mapped) for(auto &&x:r) out.emplace_back(std::forward<decltype(x)>(x)); return make(std::move(out)); }
        [[nodiscard]] auto take(usize n) { auto r=std::views::take(std::move(view_),static_cast<std::ranges::range_difference_t<V>>(n)); return ParIter<Rt,decltype(r)>(std::move(r)); }
        [[nodiscard]] auto skip(usize n) { auto r=std::views::drop(std::move(view_),static_cast<std::ranges::range_difference_t<V>>(n)); return ParIter<Rt,decltype(r)>(std::move(r)); }
        [[nodiscard]] auto step_by(usize n) { auto r=std::views::stride(std::move(view_),static_cast<std::ranges::range_difference_t<V>>(n)); return ParIter<Rt,decltype(r)>(std::move(r)); }
        [[nodiscard]] auto rev() { auto r=std::views::reverse(std::move(view_)); return ParIter<Rt,decltype(r)>(std::move(r)); }
        [[nodiscard]] auto enumerate() { return make(Ranges::map<Rt>(view_, [i=usize{0}](auto &&x) mutable { return std::pair<usize,std::ranges::range_value_t<V>>{i++,x}; })); }
        template <std::ranges::viewable_range R2> [[nodiscard]] auto zip(R2 &&other) { return make(Ranges::map<Rt>(view_, [other=std::views::all(std::forward<R2>(other))](auto &&x) mutable { static usize i=0; return std::pair{std::forward<decltype(x)>(x),other[i++]}; })); }

        template <class Fn> requires AsyncWork<Fn, std::ranges::range_reference_t<V>> void for_each(Fn fn) { Ranges::for_each<Rt>(view_,std::move(fn)); }
        template <class T, class Fn> requires AsyncWork<Fn,T,std::ranges::range_reference_t<V>> [[nodiscard]] T fold(T init,Fn fn) { return reduce(std::move(init),std::move(fn)); }
        template <class T, class Fn> requires AsyncWork<Fn,T,std::ranges::range_reference_t<V>> [[nodiscard]] T reduce(T identity,Fn combine) {
            const usize size=count(); if(!size)return identity; const auto chunks=Detail::chunk_bounds(size,Detail::chunk_count_for<Rt>(size)); auto first=std::ranges::begin(view_); using D=std::ranges::range_difference_t<V>; std::vector<TaskHandle<T>> handles; for(const auto &c:chunks){auto b=first+static_cast<D>(c.begin);auto e=first+static_cast<D>(c.end);handles.push_back(Rt::spawn([b,e,identity,combine]{T a=identity;for(auto i=b;i!=e;++i)a=combine(std::move(a),*i);return a;}));} T result=identity;for(auto &h:handles)result=combine(std::move(result),h.wait());return result;
        }
        [[nodiscard]] auto sum(){using T=std::ranges::range_value_t<V>;return reduce(T{},std::plus<>{});}
        [[nodiscard]] auto product(){using T=std::ranges::range_value_t<V>;return reduce(T{1},std::multiplies<>{});}
        template <class Pred> [[nodiscard]] bool any(Pred p){return Ranges::any<Rt>(view_,std::move(p));}
        template <class Pred> [[nodiscard]] bool all(Pred p){return Ranges::all<Rt>(view_,std::move(p));}
        template <class Pred> [[nodiscard]] auto find(Pred p){return Ranges::find<Rt>(view_,std::move(p));}
        template <template<class...> class Container=std::vector> [[nodiscard]] auto collect(){using T=std::ranges::range_value_t<V>;Container<T> out;out.reserve(count());for(auto &&x:view_)out.emplace_back(x);return out;}
    };

    namespace Detail {
        template <AsyncRuntime Rt>
        struct ParIterFn : std::ranges::range_adaptor_closure<ParIterFn<Rt>> {
            template <ParIterable R> [[nodiscard]] constexpr auto operator()(R &&range) const { auto v=std::views::all(std::forward<R>(range)); return ParIter<Rt,decltype(v)>(std::move(v)); }
        };
    }

    inline constexpr Detail::ParIterFn<DefaultRuntime> par_iter{};
    template <AsyncRuntime Rt=DefaultRuntime> inline constexpr Detail::ParIterFn<Rt> par_iter_on{};

} // namespace SFT::Async
