#ifndef INCLUDED__csp__internal__function_h
#define INCLUDED__csp__internal__function_h

#include <functional>
#include <type_traits>
#include <tuple>

namespace csp {

    namespace detail {

        template <typename T> struct remove_class { };
        template <typename C, typename R, typename... A> struct remove_class<R(C::*)(A...)               > { using type = R(A...); };
        template <typename C, typename R, typename... A> struct remove_class<R(C::*)(A...) const         > { using type = R(A...); };
        template <typename C, typename R, typename... A> struct remove_class<R(C::*)(A...)       volatile> { using type = R(A...); };
        template <typename C, typename R, typename... A> struct remove_class<R(C::*)(A...) const volatile> { using type = R(A...); };

        template <typename T> using remove_class_t = typename remove_class<T>::type;

        template<typename T, typename = void>
        struct function_signature_impl { // For functors/lambdas
            using type = remove_class_t<decltype(&std::remove_reference_t<T>::operator())>;
        };

        template<typename T>
        struct function_signature_impl<T, std::enable_if_t<std::is_function_v<T>>> { // For function types R(Args...)
            using type = T;
        };
    }

    template<typename T>
    struct function_signature {
        using type = typename detail::function_signature_impl<
            std::remove_pointer_t<
                std::remove_cv_t<
                    std::remove_reference_t<T>
                >
            >
        >::type;
    };

    template <typename F> using function_signature_t = typename function_signature<F>::type;

    template<typename F> using make_function_type = std::function<function_signature_t<F>>;

    template<typename F> make_function_type<F> make_function(F &&f) {
        return make_function_type<F>(std::forward<F>(f));
    }

    template <typename F, typename Tuple>
    auto apply(F && f, Tuple && t) {
        return std::apply(std::forward<F>(f), std::forward<Tuple>(t));
    }

}

#endif // INCLUDED__csp__internal__function_h
