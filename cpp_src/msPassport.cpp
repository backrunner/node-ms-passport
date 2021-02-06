#include <napi.h>
#include <sstream>
#include <random>
#include <utility>
#include <iostream>

#include "NodeMsPassport.hpp"

#define CHECK_ARGS(...) ::util::checkArgs(info, ::util::removeNamespace(__FUNCTION__), {__VA_ARGS__})
/*#define CHECK_INDEX(index, size) if (index < 0) throw Napi::RangeError::New(env, "Negative index requested"); \
                    else if (((size_t) index) >= size) throw Napi::RangeError::New(env, "Index out of range. Requested index: " + std::to_string(index) + ", array size: " + std::to_string(size))*/
#define CATCH_EXCEPTIONS catch (const std::exception &e) {throw Napi::Error::New(info.Env(), e.what());} catch (...) {throw Napi::Error::New(info.Env(), "An unknown exception occurred");}
#define EXPORT(function) "js_" + ::util::removeNamespace(#function), Napi::Function::New(env, function)

using namespace nodeMsPassport;

enum type {
    STRING,
    NUMBER,
    FUNCTION,
    OBJECT,
    BOOLEAN
};

class exception : public std::exception {
public:
#ifdef __APPLE__
    explicit exception(const char *msg) : std::exception(), msg(msg) {}

        [[nodiscard]] const char *what() const noexcept override {
            return msg;
        }

    private:
        const char *msg;
#else
    using std::exception::exception;
#endif
};

namespace util {
    std::string removeNamespace(const std::string &str) {
        return str.substr(str.rfind(':') + 1);
    }

    void checkArgs(const Napi::CallbackInfo &info, const std::string &funcName, const std::vector<type> &types) {
        Napi::Env env = info.Env();
        if (info.Length() < types.size()) {
            throw Napi::TypeError::New(env, funcName + " requires " + std::to_string(types.size()) + " arguments");
        }

        for (size_t i = 0; i < types.size(); i++) {
            if (types[i] == STRING) {
                if (!info[i].IsString()) {
                    throw Napi::TypeError::New(env, "Argument type mismatch: " + funcName +
                                                    " requires type string at position " + std::to_string(i + 1));
                }
            } else if (types[i] == NUMBER) {
                if (!info[i].IsNumber()) {
                    throw Napi::TypeError::New(env, "Argument type mismatch: " + funcName +
                                                    " requires type number at position " + std::to_string(i + 1));
                }
            } else if (types[i] == FUNCTION) {
                if (!info[i].IsFunction()) {
                    throw Napi::TypeError::New(env, "Argument type mismatch: " + funcName +
                                                    " requires type function at position " + std::to_string(i + 1));
                }
            } else if (types[i] == OBJECT) {
                if (!info[i].IsObject()) {
                    throw Napi::TypeError::New(env, "Argument type mismatch: " + funcName +
                                                    " requires type object at position " + std::to_string(i + 1));
                }
            } else if (types[i] == BOOLEAN) {
                if (!info[i].IsBoolean()) {
                    throw Napi::TypeError::New(env, "Argument type mismatch: " + funcName +
                                                    " requires type boolean at position " + std::to_string(i + 1));
                }
            }
        }
    }
}

passport::util::secure_byte_vector string_to_binary(const std::string &source) {
    static unsigned int nibbles[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0, 0, 10, 11, 12, 13, 14, 15};
    passport::util::secure_byte_vector retval;
    for (std::string::const_iterator it = source.begin(); it < source.end(); it += 2) {
        unsigned char v;
        if (isxdigit(*it))
            v = nibbles[toupper(*it) - '0'] << (unsigned) 4;
        else {
            std::string err = "Invalid character: '";
            err += (char) *it;
            err.append("' is not a valid hex digit");
            throw std::exception(err.c_str());
        }
        if (it + 1 < source.end() && isxdigit(*(it + 1)))
            v += nibbles[toupper(*(it + 1)) - '0'];
        retval.push_back(v);
    }
    return retval;
}

std::string binary_to_string(const passport::util::secure_byte_vector &source) {
    static char syms[] = "0123456789ABCDEF";
    std::stringstream ss;
    for (std::_Vector_const_iterator<std::_Vector_val<std::_Simple_types<char> > >::value_type it : source)
        ss << syms[(((unsigned) it >> (unsigned) 4) & (unsigned) 0xf)] << syms[(unsigned) it & (unsigned) 0xf];

    return ss.str();
}

namespace workers {
    class createPassportKeyWorker : public Napi::AsyncWorker {
    public:
        createPassportKeyWorker(const Napi::Env &env, std::string account) : Napi::AsyncWorker(env),
                                                                             deferred(
                                                                                     Napi::Promise::Deferred::New(env)),
                                                                             account(std::move(account)), status(1) {}

        ~createPassportKeyWorker() override = default;

        void Execute() override {
            try {
                passport::OperationResult res = passport::createPassportKey(account);
                if (res.ok()) data = binary_to_string(res.data);
                status = res.status;
            } catch (const std::exception &e) {
                Napi::AsyncWorker::SetError(e.what());
            } catch (...) {
                Napi::AsyncWorker::SetError("An unknown exception occurred");
            }
        }

        void OnOK() override {
            Napi::Object obj = Napi::Object::New(Env());
            obj.Set("status", Napi::Number::New(Env(), status));
            obj.Set("ok", Napi::Boolean::New(Env(), status == 0));
            if (status == 0) {
                obj.Set("data", Napi::String::New(Env(), data));
            } else {
                obj.Set("data", Env().Null());
            }

            deferred.Resolve(obj);
        }

        void OnError(Napi::Error const &error) override {
            deferred.Reject(error.Value());
        }

        Napi::Promise GetPromise() {
            return deferred.Promise();
        }

    private:
        Napi::Promise::Deferred deferred;
        std::string account;
        int status;
        std::string data;
    };

    class passportSignWorker : public Napi::AsyncWorker {
    public:
        passportSignWorker(const Napi::Env &env, std::string account, std::string challenge) : Napi::AsyncWorker(env),
                                                                                               deferred(
                                                                                                       Napi::Promise::Deferred::New(
                                                                                                               env)),
                                                                                               account(std::move(
                                                                                                       account)),
                                                                                               challenge(std::move(
                                                                                                       challenge)),
                                                                                               status(1) {}

        ~passportSignWorker() override = default;

        void Execute() override {
            try {
                passport::util::secure_byte_vector challenge_vec = string_to_binary(challenge);

                passport::OperationResult res = passport::passportSign(account, challenge_vec);

                if (res.ok()) data = binary_to_string(res.data);
                status = res.status;
            } catch (const std::exception &e) {
                Napi::AsyncWorker::SetError(e.what());
            } catch (...) {
                Napi::AsyncWorker::SetError("An unknown exception occurred");
            }
        }

        void OnOK() override {
            Napi::Object obj = Napi::Object::New(Env());
            obj.Set("status", Napi::Number::New(Env(), status));
            obj.Set("ok", Napi::Boolean::New(Env(), status == 0));
            if (status == 0) {
                obj.Set("data", Napi::String::New(Env(), data));
            } else {
                obj.Set("data", Env().Null());
            }

            deferred.Resolve(obj);
        }

        void OnError(Napi::Error const &error) override {
            deferred.Reject(error.Value());
        }

        Napi::Promise GetPromise() {
            return deferred.Promise();
        }

    private:
        Napi::Promise::Deferred deferred;
        std::string account, challenge;
        int status;
        std::string data;
    };
}

Napi::Object convertToObject(const Napi::Env &env, const passport::OperationResult &res) {
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("status", Napi::Number::New(env, res.status));
    obj.Set("ok", Napi::Boolean::New(env, res.ok()));
    if (res.status == 0) {
        obj.Set("data", Napi::String::New(env, binary_to_string(res.data)));
    } else {
        obj.Set("data", env.Null());
    }

    return obj;
}

Napi::Boolean passportAvailable(const Napi::CallbackInfo &info) {
    try {
        return Napi::Boolean::New(info.Env(), passport::passportAvailable());
    } CATCH_EXCEPTIONS
}

Napi::Object createPassportKey(const Napi::CallbackInfo &info) {
    CHECK_ARGS(STRING);

    try {
        std::string account = info[0].As<Napi::String>();
        passport::OperationResult res = passport::createPassportKey(account);

        return convertToObject(info.Env(), res);
    } CATCH_EXCEPTIONS
}

Napi::Promise createPassportKeyAsync(const Napi::CallbackInfo &info) {
    CHECK_ARGS(STRING);
    Napi::Env env = info.Env();
    std::string account = info[0].As<Napi::String>();

    // Napi destroys this object
    auto *worker = new workers::createPassportKeyWorker(env, account);
    Napi::Promise promise = worker->GetPromise();

    worker->Queue();

    return promise;
}

Napi::Object passportSign(const Napi::CallbackInfo &info) {
    CHECK_ARGS(STRING, STRING);

    try {
        std::string account = info[0].As<Napi::String>();
        passport::util::secure_byte_vector challenge = string_to_binary(info[1].As<Napi::String>().Utf8Value());

        passport::OperationResult res = passport::passportSign(account, challenge);

        return convertToObject(info.Env(), res);
    } CATCH_EXCEPTIONS
}

Napi::Object passportSignAsync(const Napi::CallbackInfo &info) {
    CHECK_ARGS(STRING, STRING);

    Napi::Env env = info.Env();
    std::string account = info[0].As<Napi::String>();
    std::string challenge = info[1].As<Napi::String>();

    // Napi destroys this object
    auto *worker = new workers::passportSignWorker(env, account, challenge);
    Napi::Promise promise = worker->GetPromise();

    worker->Queue();

    return promise;
}

Napi::Number deletePassportAccount(const Napi::CallbackInfo &info) {
    CHECK_ARGS(STRING);

    try {
        std::string account = info[0].As<Napi::String>();

        return Napi::Number::New(info.Env(), passport::deletePassportAccount(account));
    } CATCH_EXCEPTIONS
}

Napi::Object getPublicKey(const Napi::CallbackInfo &info) {
    CHECK_ARGS(STRING);

    try {
        std::string account = info[0].As<Napi::String>();

        passport::OperationResult res = passport::getPublicKey(account);

        return convertToObject(info.Env(), res);
    } CATCH_EXCEPTIONS
}

Napi::Object getPublicKeyHash(const Napi::CallbackInfo &info) {
    CHECK_ARGS(STRING);

    try {
        std::string account = info[0].As<Napi::String>();

        passport::OperationResult res = passport::getPublicKeyHash(account);

        return convertToObject(info.Env(), res);
    } CATCH_EXCEPTIONS
}

Napi::Boolean verifySignature(const Napi::CallbackInfo &info) {
    CHECK_ARGS(STRING, STRING, STRING);

    try {
        passport::util::secure_byte_vector challenge = string_to_binary(info[0].As<Napi::String>().Utf8Value());
        passport::util::secure_byte_vector signature = string_to_binary(info[1].As<Napi::String>().Utf8Value());
        passport::util::secure_byte_vector publicKey = string_to_binary(info[2].As<Napi::String>().Utf8Value());

        return Napi::Boolean::New(info.Env(), passport::verifySignature(challenge, signature, publicKey));
    } CATCH_EXCEPTIONS
}

Napi::Boolean writeCredential(const Napi::CallbackInfo &info) {
    CHECK_ARGS(STRING, STRING, STRING, BOOLEAN);

    try {
        std::u16string target_u16 = info[0].As<Napi::String>();
        std::u16string user_u16 = info[1].As<Napi::String>();
        std::u16string password_u16 = info[2].As<Napi::String>();

        std::wstring target(target_u16.begin(), target_u16.end());
        std::wstring user(user_u16.begin(), user_u16.end());
        secure_wstring password(password_u16.begin(), password_u16.end());

        return Napi::Boolean::New(info.Env(), credentials::write(target, user, password, info[3].As<Napi::Boolean>()));
    } CATCH_EXCEPTIONS
}

Napi::Value readCredential(const Napi::CallbackInfo &info) {
    CHECK_ARGS(STRING, BOOLEAN);

    try {
        Napi::Env env = info.Env();
        std::u16string target_utf16 = info[0].As<Napi::String>().Utf16Value();
        std::wstring target(target_utf16.begin(), target_utf16.end());
        std::wstring user;
        secure_wstring password;

        if (credentials::read(target, user, password, info[1].As<Napi::Boolean>())) {
            Napi::Object obj = Napi::Object::New(env);

            obj.Set("username", Napi::String::New(env, std::u16string(user.begin(), user.end())));
            obj.Set("password", Napi::String::New(env, std::u16string(password.begin(), password.end())));
            return obj;
        } else {
            return env.Null();
        }
    } CATCH_EXCEPTIONS
}

Napi::Boolean removeCredential(const Napi::CallbackInfo &info) {
    CHECK_ARGS(STRING);

    try {
        std::u16string target = info[0].As<Napi::String>();

        return Napi::Boolean::New(info.Env(), credentials::remove(std::wstring(target.begin(), target.end())));
    } CATCH_EXCEPTIONS
}

Napi::Boolean credentialEncrypted(const Napi::CallbackInfo &info) {
    CHECK_ARGS(STRING);

    try {
        Napi::Env env = info.Env();
        std::u16string target = info[0].As<Napi::String>();

        bool res = credentials::isEncrypted(std::wstring(target.begin(), target.end()));

        return Napi::Boolean::New(env, res);
    } CATCH_EXCEPTIONS
}

Napi::String encryptPassword(const Napi::CallbackInfo &info) {
    CHECK_ARGS(STRING);

    try {
        Napi::Env env = info.Env();
        std::u16string data_u16 = info[0].As<Napi::String>();
        secure_wstring data(data_u16.begin(), data_u16.end());

        bool ok = passwords::encrypt(data);

        if (!ok) throw exception("Could not encrypt the data");
        else return Napi::String::New(env, binary_to_string(data.getBytes()));
    } CATCH_EXCEPTIONS
}

Napi::String decryptPassword(const Napi::CallbackInfo &info) {
    CHECK_ARGS(STRING);

    try {
        Napi::Env env = info.Env();
        std::string data_str = info[0].As<Napi::String>();
        secure_wstring data(string_to_binary(data_str));

        bool ok = passwords::decrypt(data);

        if (!ok) throw exception("Could not decrypt the data");
        else return Napi::String::New(env, std::u16string(data.begin(), data.end()));
    } CATCH_EXCEPTIONS
}

Napi::Boolean passwordEncrypted(const Napi::CallbackInfo &info) {
    CHECK_ARGS(STRING);

    try {
        Napi::Env env = info.Env();
        std::string password = info[0].As<Napi::String>();

        secure_vector<unsigned char> data_vec = string_to_binary(password);
        secure_wstring data(data_vec);

        bool res = passwords::isEncrypted(data);

        return Napi::Boolean::New(env, res);
    } CATCH_EXCEPTIONS
}

Napi::String generateRandom(const Napi::CallbackInfo &info) {
    CHECK_ARGS(NUMBER);

    try {
        int numChars = info[0].As<Napi::Number>();

        std::random_device dev;
        std::mt19937 rng(dev());
        std::uniform_int_distribution<> dist(0, UCHAR_MAX);

        passport::util::secure_byte_vector buffer;
        buffer.reserve(numChars);
        for (int i = 0; i < numChars; i++) {
            buffer.push_back((unsigned char) dist(rng));
        }

        return Napi::String::New(info.Env(), binary_to_string(buffer));
    } CATCH_EXCEPTIONS
}

void setCSharpDllLocation(const Napi::CallbackInfo &info) {
    CHECK_ARGS(STRING);

    try {
        passport::setCSharpDllLocation(info[0].ToString().Utf8Value());
    } CATCH_EXCEPTIONS
}

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
    exports.Set(EXPORT(passportAvailable));
    exports.Set(EXPORT(createPassportKey));
    exports.Set(EXPORT(createPassportKeyAsync));
    exports.Set(EXPORT(passportSign));
    exports.Set(EXPORT(passportSignAsync));
    exports.Set(EXPORT(deletePassportAccount));
    exports.Set(EXPORT(getPublicKey));
    exports.Set(EXPORT(getPublicKeyHash));
    exports.Set(EXPORT(verifySignature));

    exports.Set(EXPORT(writeCredential));
    exports.Set(EXPORT(readCredential));
    exports.Set(EXPORT(removeCredential));
    exports.Set(EXPORT(credentialEncrypted));

    exports.Set(EXPORT(encryptPassword));
    exports.Set(EXPORT(decryptPassword));
    exports.Set(EXPORT(passwordEncrypted));

    exports.Set(EXPORT(generateRandom));
    exports.Set(EXPORT(setCSharpDllLocation));

    return exports;
}

NODE_API_MODULE(passport, InitAll)