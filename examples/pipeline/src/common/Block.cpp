#include <typeinfo>
#include <algorithm>
#include <map>
#include <thread>
#include <mutex>
#include <vector>
#include <functional>
#include <sstream>
#include <aff3ct.hpp>

#include "Block.hpp"
#include "Buffered_Socket.hpp"

Block
::Block(const aff3ct::module::Task &task, const size_t buffer_size, const size_t n_threads)
: name(task.get_name()),
  n_threads(n_threads),
  buffer_size(buffer_size),
  threads(n_threads)
{
	if (n_threads == 0)
	{
		std::stringstream message;
		message << "'n_threads' has to be strictly positive ('n_threads' = " << n_threads << ").";
		throw aff3ct::tools::invalid_argument(__FILE__, __LINE__, __func__, message.str());
	}

	if (buffer_size < n_threads)
	{
		std::stringstream message;
		message << "'buffer_size' has to be equal or greater than 'n_threads' ("
		        << "'buffer_size'" << " = " << buffer_size << ", "
		        << "'n_threads'"   << " = " << n_threads
		        << ").";
		throw aff3ct::tools::invalid_argument(__FILE__, __LINE__, __func__, message.str());
	}

	auto task_cpy = std::unique_ptr<aff3ct::module::Task>(task.clone());
	task_cpy->set_autoalloc(false);
	task_cpy->set_autoexec (false);
	task_cpy->set_fast     (false);

	for (int i = 0; i < n_threads; i++)
		tasks.push_back(std::shared_ptr<aff3ct::module::Task>(task_cpy->clone()));

	for (size_t s_idx = 0; s_idx < task.sockets.size(); s_idx++)
	{
		std::vector<std::shared_ptr<aff3ct::module::Socket>> s_vec;
		for (int i = 0 ; i < n_threads; i++)
			s_vec.push_back(tasks[i]->sockets[s_idx]);

		std::shared_ptr<aff3ct::module::Socket> s = task.sockets[s_idx];
		const auto sdatatype = s->get_datatype_string();
		const auto sname = s->get_name();
		const auto stype = task.get_socket_type(*s);

		std::function<void(NT_Buffered_Socket*)> add_socket;
		if (stype == aff3ct::module::socket_t::SIN)
			add_socket = [this, sname](NT_Buffered_Socket* socket) {
				this->buffered_sockets_in[sname] = std::unique_ptr<NT_Buffered_Socket>(socket);
			};
		else
			add_socket = [this, sname](NT_Buffered_Socket* socket) {
				this->buffered_sockets_out[sname] = std::unique_ptr<NT_Buffered_Socket>(socket);
			};

		     if (sdatatype == "int8"   ) add_socket(new Buffered_Socket<int8_t >(s_vec, stype, buffer_size));
		else if (sdatatype == "int16"  ) add_socket(new Buffered_Socket<int16_t>(s_vec, stype, buffer_size));
		else if (sdatatype == "int32"  ) add_socket(new Buffered_Socket<int32_t>(s_vec, stype, buffer_size));
		else if (sdatatype == "int64"  ) add_socket(new Buffered_Socket<int64_t>(s_vec, stype, buffer_size));
		else if (sdatatype == "float32") add_socket(new Buffered_Socket<float  >(s_vec, stype, buffer_size));
		else if (sdatatype == "float64") add_socket(new Buffered_Socket<double >(s_vec, stype, buffer_size));
	}
}

template <typename T>
int Block
::bind_by_type(const std::string &start_sck_name, Block &dest_block, const std::string &dest_sck_name)
{
	auto sin = this->get_buffered_socket_in<T>(start_sck_name);
	auto sout = dest_block.get_buffered_socket_out<T>(dest_sck_name);
	return sin->bind(sout);
}

int Block
::bind(const std::string &start_sck_name, Block &dest_block, const std::string &dest_sck_name)
{
	if (!this->buffered_sockets_in.count(start_sck_name))
	{
		std::stringstream message;
		message << "'buffered_sockets_in.count(start_sck_name)' has to be strictly positive ("
		        << "'start_sck_name'"                            << " = " << start_sck_name << ", "
		        << "'buffered_sockets_in.count(start_sck_name)'" << " = " << buffered_sockets_in.count(start_sck_name)
		        << ").";
		throw aff3ct::tools::runtime_error(__FILE__, __LINE__, __func__, message.str());
	}

	auto sin_datatype = this->buffered_sockets_in[start_sck_name]->get_datatype();
	     if (sin_datatype == typeid(int8_t )) return bind_by_type<int8_t >(start_sck_name, dest_block, dest_sck_name);
	else if (sin_datatype == typeid(int16_t)) return bind_by_type<int16_t>(start_sck_name, dest_block, dest_sck_name);
	else if (sin_datatype == typeid(int32_t)) return bind_by_type<int32_t>(start_sck_name, dest_block, dest_sck_name);
	else if (sin_datatype == typeid(int64_t)) return bind_by_type<int64_t>(start_sck_name, dest_block, dest_sck_name);
	else if (sin_datatype == typeid(float  )) return bind_by_type<float  >(start_sck_name, dest_block, dest_sck_name);
	else if (sin_datatype == typeid(double )) return bind_by_type<double >(start_sck_name, dest_block, dest_sck_name);

	std::stringstream message;
	message << "This should not happen :-(.";
	throw aff3ct::tools::runtime_error(__FILE__, __LINE__, __func__, message.str());
}

void Block
::run(const bool &is_done)
{
	for (size_t tid = 0; tid < this->n_threads; tid++)
		this->threads[tid] = std::thread(&Block::execute_task, this, tid, std::ref(is_done));
};

void Block
::join()
{
	for (auto &th : this->threads)
		th.join();
};

void Block
::execute_task(const size_t tid, const bool &is_done)
{
	while (!is_done)
	{
		for (auto const& it : this->buffered_sockets_in)
			while (!is_done && it.second->pop(tid)){};

		if (is_done)
			break;

		this->tasks[tid]->exec();

		for (auto const& it : this->buffered_sockets_out)
			while (!is_done && it.second->push(tid)){};
	}

	for (auto const& it : this->buffered_sockets_out) { it.second->stop(); }
	for (auto const& it : this->buffered_sockets_in ) { it.second->stop(); }
}

void Block
::reset()
{
	for (auto const& it : this->buffered_sockets_in ) { it.second->reset(); }
	for (auto const& it : this->buffered_sockets_out) { it.second->reset(); }
}

template <typename T>
Buffered_Socket<T>* Block
::get_buffered_socket_in(std::string name)
{
	if (this->buffered_sockets_in.count(name))
	{
		if (aff3ct::module::type_to_string[this->buffered_sockets_in[name]->get_socket()->get_datatype()] ==
		    aff3ct::module::type_to_string[typeid(T)])
		{
			return static_cast<Buffered_Socket<T>*>(this->buffered_sockets_in[name].get());
		}
		else
		{
			std::stringstream message;
			message << "'buffered_sockets_in[name]->get_socket()->get_datatype()' has to be equal to 'typeid(T)' ("
			        << "'name'" << " = " << name << ", "
			        << "'buffered_sockets_in[name]->get_socket()->get_datatype()'" << " = "
			        << aff3ct::module::type_to_string[this->buffered_sockets_in[name]->get_socket()->get_datatype()]
			        << ", "
			        << "'typeid(T)'" << " = " << aff3ct::module::type_to_string[typeid(T)]
			        << ").";
			throw aff3ct::tools::runtime_error(__FILE__, __LINE__, __func__, message.str());
		}
	}
	else
	{
		std::stringstream message;
		message << "'buffered_sockets_in.count(name)' has to be strictly positive ("
		        << "'name'"                            << " = " << name << ", "
		        << "'buffered_sockets_in.count(name)'" << " = " << buffered_sockets_in.count(name)
		        << ").";
		throw aff3ct::tools::runtime_error(__FILE__, __LINE__, __func__, message.str());
	}
}

template <typename T>
Buffered_Socket<T>* Block
::get_buffered_socket_out(std::string name)
{
	if (this->buffered_sockets_out.count(name))
	{
		if (aff3ct::module::type_to_string[this->buffered_sockets_out[name]->get_socket()->get_datatype()] ==
		    aff3ct::module::type_to_string[typeid(T)])
		{
			return static_cast<Buffered_Socket<T>*>(this->buffered_sockets_out[name].get());
		}
		else
		{
			std::stringstream message;
			message << "'buffered_sockets_out[name]->get_socket()->get_datatype()' has to be equal to 'typeid(T)' ("
			        << "'name'" << " = " << name << ", "
			        << "'buffered_sockets_out[name]->get_socket()->get_datatype()'" << " = "
			        << aff3ct::module::type_to_string[this->buffered_sockets_out[name]->get_socket()->get_datatype()]
			        << ", "
			        << "'typeid(T)'" << " = " << aff3ct::module::type_to_string[typeid(T)]
			        << ").";
			throw aff3ct::tools::runtime_error(__FILE__, __LINE__, __func__, message.str());
		}
	}
	else
	{
		std::stringstream message;
		message << "'buffered_sockets_out.count(name)' has to be strictly positive ("
		        << "'name'"                             << " = " << name << ", "
		        << "'buffered_sockets_out.count(name)'" << " = " << buffered_sockets_out.count(name)
		        << ").";
		throw aff3ct::tools::runtime_error(__FILE__, __LINE__, __func__, message.str());
	}
}