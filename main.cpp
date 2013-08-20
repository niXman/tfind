
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>

#include <boost/thread/thread.hpp>
#include <boost/thread/future.hpp>

#include <boost/interprocess/ipc/message_queue.hpp>

#include <boost/regex.hpp>

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/classification.hpp>

#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>

namespace po = boost::program_options;
namespace ip = boost::interprocess;
namespace fs = boost::filesystem;

/***************************************************************************/

struct position {
	position()
		:line(0)
		,col(0)
	{}

	position(std::size_t line, std::size_t col)
		:line(line)
		,col(col)
	{}

	position(const position &p)
		:line(p.line)
		,col(p.col)
	{}

	std::size_t line;
	std::size_t col;
};

typedef std::unordered_map<
	 std::string // file name
	,std::vector<position> // positions
> search_results;

/***************************************************************************/

namespace {

static const char* magic_string = "\\_magic_string_\\";
static const char* message_queue_name = "super_unique_message_queue_name";
static const std::size_t max_queue_size = 1024;
static const std::size_t max_queue_item_size = 1024*2;

} // ns

/***************************************************************************/

#define PRINT_STD_EXCEPTION(ex) \
	std::cerr \
	<< "[exception] " \
	<< "message: \"" << ex.what() << "\"" << std::endl;

#define PRINT_INTERPROCESS_EXCEPTION(ex) \
	std::cerr \
	<< "[exception] " \
	<< "message: \"" << ex.what() << "\", " \
	<< "error code: " << ex.get_error_code() << ", " \
	<< "native error: " << ex.get_native_error() << std::endl;

/***************************************************************************/

void read_tree_thread(
	const bool recursive,
	const std::string &path,
	const std::string &mask,
	boost::promise<std::size_t>& promise
) {
	boost::shared_ptr<ip::message_queue> queue;
	std::size_t count = 0;

	std::vector<std::string> masks;
	boost::algorithm::split(masks, mask, boost::algorithm::is_any_of(";"));

	for ( std::string &it: masks) {
		boost::replace_all(it, "\\", "\\\\");
		boost::replace_all(it, "^", "\\^");
		boost::replace_all(it, ".", "\\.");
		boost::replace_all(it, "$", "\\$");
		boost::replace_all(it, "|", "\\|");
		boost::replace_all(it, "(", "\\(");
		boost::replace_all(it, ")", "\\)");
		boost::replace_all(it, "[", "\\[");
		boost::replace_all(it, "]", "\\]");
		boost::replace_all(it, "*", ".*");
		boost::replace_all(it, "+", "\\+");
		boost::replace_all(it, "?", ".");
		boost::replace_all(it, "/", "\\/");
	}

	auto functor = [&count, &queue, &masks](const boost::filesystem::directory_entry& iter) {
		const std::string &item = iter.path().string();
		if ( item.length() >= max_queue_item_size ) {
			std::cout
				<< "directory item(" << item << ") greater then max allowed item size("
				<< max_queue_item_size << ")"
			<< std::endl;
			return;
		}

		for ( const std::string& it: masks ) {
			boost::regex pattern(it, boost::regex::icase);
			const bool ok = boost::regex_match(item, pattern);
			if ( ok ) break;
			if ( !ok && it == masks.back() ) {
				return;
			}
		}

		bool ok;
		do {
			ok = queue->try_send(
				item.c_str(),
				item.length()+1,
				0
			);

			if ( !ok ) {
				boost::this_thread::sleep(boost::posix_time::milliseconds(1));
			} else {
				std::cout << "send    :" << item << std::endl;
			}
		} while ( !ok );

		++count;
	};

	try {
		queue = boost::make_shared<ip::message_queue>(
			ip::open_only,
			message_queue_name
		);

		if ( recursive ) {
			fs::recursive_directory_iterator beg(path), end;
			std::for_each(beg, end, functor);
		} else {
			fs::directory_iterator beg(path), end;
			std::for_each(beg, end, functor);
		}

		queue->send(magic_string, strlen(magic_string)+1, 0);
	} catch ( ... ) {
		promise.set_exception(boost::current_exception());
		return;
	}

	promise.set_value(count);
}

/***************************************************************************/

void file_grep_thread(const std::string &regexp, search_results &results, boost::promise<std::size_t>& promise) {
	boost::shared_ptr<ip::message_queue> queue;
	std::size_t count = 0;
	try {
		queue = boost::make_shared<ip::message_queue>(
			ip::open_only,
			message_queue_name
		);

		char buf[max_queue_item_size];
		while ( true ) {
			std::size_t recv_size;
			unsigned priority;
			queue->receive(buf, max_queue_item_size, recv_size, priority);
			if ( 0 == strncmp(buf, magic_string, max_queue_item_size) ) break;

			std::cout << "received:" << buf << std::endl;

			/**
			 * read file
			 * search
			 * fill resuts
			 */

			++count;
		}

	} catch ( ... ) {
		promise.set_exception(boost::current_exception());
		return;
	}

	promise.set_value(count);
}

/***************************************************************************/

int main(int argc, char *argv[]) {
	/**  */
	std::string path, mask, text;
	bool recursive = false;

	/**  */
	po::options_description descriptions("allowed options");
	descriptions.add_options()
		("help,h", "produce help message")
		("path,p", po::value<std::string>(&path)->default_value("."), "start from path")
		("file,f", po::value<std::string>(&mask)->required(), "file patterns separated by ';'")
		("text,t", po::value<std::string>(&text)->required(), "string expression")
		("recursive,r", "find recursively")
	;
	po::variables_map options;
	try {
		po::store(po::parse_command_line(argc, argv, descriptions), options);
		po::notify(options);
	} catch ( const std::exception& ex ) {
		PRINT_STD_EXCEPTION(ex);
		return 0;
	}
	if ( options.count("help") ) {
		std::cout << descriptions << std::endl;
		return 0;
	}

	if ( options.count("recursive") ) {
		recursive = true;
	}

	if ( !fs::exists(path) || !fs::is_directory(path) ) {
		std::cerr << "\"" << path << "\" is not a directory! terminate..." << std::endl;
		return 1;
	}

	ip::message_queue::remove(message_queue_name);

	boost::shared_ptr<ip::message_queue> queue;
	try {
		queue = boost::make_shared<ip::message_queue>(
			ip::create_only,
			message_queue_name,
			max_queue_size,
			max_queue_item_size
		);
	} catch ( const ip::interprocess_exception& ex ) {
		PRINT_INTERPROCESS_EXCEPTION(ex);
		return 1;
	}

	boost::promise<std::size_t> read_tree_promise;
	boost::unique_future<std::size_t> read_tree_future = read_tree_promise.get_future();
	boost::promise<std::size_t> file_grep_promise;
	boost::unique_future<std::size_t> file_grep_future = file_grep_promise.get_future();

	search_results results;

	boost::thread_group threads;
	threads.create_thread(
		boost::bind(
			&read_tree_thread
			,recursive
			,boost::cref(path)
			,boost::cref(mask)
			,boost::ref(read_tree_promise)
		)
	);
	threads.create_thread(
		boost::bind(
			&file_grep_thread
			,boost::cref(text)
			,boost::ref(results)
			,boost::ref(file_grep_promise)
		)
	);

	std::size_t files_count = 0, matched = 0;
	try {
		files_count = read_tree_future.get();
	} catch ( const std::exception& ex ) {
		PRINT_STD_EXCEPTION(ex);
	}
	try {
		matched = file_grep_future.get();
	} catch ( const std::exception& ex ) {
		PRINT_STD_EXCEPTION(ex);
	}

	return read_tree_future.has_exception() || file_grep_future.has_exception();
}

/***************************************************************************/
